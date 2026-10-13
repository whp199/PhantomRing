# PhantomRing host plugin (Elden Ring side)

## PhantomRingHost.dll (`er/`)

The Elden Ring half of PhantomRing: a me3 `[[natives]]` DLL (me3 0.13, profile `disable_arxan = true`,
`start_online = false`, isolated save). It implements phases 0 and 1 of `docs/DESIGN.md`: link, input
bridge, puppet, camera, dev console, debug skeleton, and the D-Walker/buddy groundwork. Config is
`PhantomRingHost.ini` next to the DLL (documented defaults in `er/PhantomRingHost.ini`; without the file
the same defaults apply; the build copies `er/PhantomRingHost.ini` to `build/` whenever it changes, so
edit that one). The log is `PhantomRingHost.log` next to the DLL.

### Start-up (`er/main.cpp`)

`DllMain` only starts a thread. That thread runs these steps in order:

1. **Exe identity.** It checks TimeDateStamp `0x6A96B418` and SizeOfImage `0x5E0DA00`. With `verify_sha256`, it also checks the sha256. Any other exe gets one `exe mismatch` line in the log, and the DLL stays inert.
2. **Address table** (`er/Addresses.cpp`). Each entry is checked against this exe:
   - globals: a unique AOB that resolves to the expected RVA;
   - functions: the expected bytes at the RVA (for jmp thunks, the jump target too);
   - vtables: the RTTI name behind `vtable[-1]`.

   A failed entry is logged and stays null. Every feature checks what it needs and turns itself off rather than guess.
3. **Shared memory** (`er/Link.cpp`). It opens or creates `/dev/shm/phantomring_v1` (OPEN_ALWAYS, full size) and clears magic while it rewrites the header. It stamps the version, a fresh BCrypt-random `sessionNonce`, the QPC frequency and `hostUnixPid` (getpid through the high syscall stub, `common/Mapping.h UnixPid`, shared with the guest). It resets every host-owned region: HostState, PadState, the host event ring, ActorTable, and the console (`respSeq = reqSeq`). It drains what it consumes: guest events and the render ring.
4. **XInput bridge** (`er/InputBridge.cpp`). MinHook detours `xinput1_4.dll` ordinal 2, the import ER uses. This is a system DLL, so nothing in `eldenring.exe` is patched.
5. **System init.** It waits for `global_hinstance` and the `CSTaskImp` instance, as fromsoftware-rs does.
6. **FD4 singleton finder** (`er/Singletons.cpp`, TGA's method in-process). It names every singleton slot through the game's own `get_singleton_name`. It cross-checks the AOB-resolved slots by name and gives the console `s.<Name>` symbols.
7. **Tasks** (`er/Tasks.cpp`). A RecurringTask-shaped object is registered with `register_task` in four groups.

### Per frame (ER task groups)

| Group | Work |
|---|---|
| FrameBegin (0) | heartbeat, guest liveness (`guestNonce == sessionNonce` and heartbeat < 3 s), guest status text → log, GuestState snapshot, frame pacing (this frame's moment on MGSV's timeline and the blended motion: Frame pacing below), menu watch, dev console, guest events, puppet start/stop, input mode, draw-list acquire, host status |
| ChrIns_PrePhysics (78) | world shift check (below), then puppet motion: (paced) odometer delta × Ry(delta) (+ X mirror), then either the kinematic controller (`er/Kinematic.cpp`: knee/chest wall rays with radius kept perpendicular to the wall, one slide along the estimated wall normal, step-up, ground snap, gravity) + `ChrIns::SetPosition`, or (`motion = root_motion`) the displacement handed to ER's own proxy as root motion (`er/RootMotion.cpp`); `ChrIns::SetYaw` |
| DrawParamUpdate (107, `[Camera] task_group`) | a fresh GuestState into the pacing history; anchor feet re-read after ER's physics; `CSCamera.pers_cam_1` ← MGSV's (paced) camera at the feet (height eased); eye→camera rays pull it in front of walls (out eased) |
| WorldChrMan_PostPhysics (117) | hide/NoDead re-asserted, HP mirror + `kHEvHurt`, `kHEvRespawn`, axis self-calibration (only while ER drives the player), ActorTable, buddy, debug draw, HostState |
| DmgMan_Post (122) | MGSV's camera into `chr_cam` once more, right before Elden Ring computes its HUD compass from it (see Compass) |
| BeginShiftWorldPosition (160), EndShiftWorldPosition (11) | the player's position at 160, compared at FrameBegin (0), 11 and ChrIns_PrePhysics (78): nothing but an origin shift moves him in between; thrown grenades, placed mines and the puppet's own stored positions move with it (`er/WorldShift.cpp`). Live (riding through Limgrave): every shift (32.00 / 32.98 / 35.78 / 40.00 m, multiples of 8 m on x/z, 9 in 10 min) landed after our task in group 11 and was caught at 78 -- before, the puppet took each for "Elden Ring moved the player 32 m by itself" and re-anchored, and mines/grenades stayed behind. A jump counts as a shift only when every axis is within 0.75 m of a whole number of 8 m steps (y too: live (48, -8, -56), (-32, -104, 16)), and the shift applied is the grid's (`WorldShift::ShiftOf`): the player's own motion of that frame no longer leaks into mines, grenades, clouds (`Explosion::OnWorldShift`) and rounds, and a console `call setpos` is no shift |

Timeline order is not index order (the static table at `.rdata 0x2C04D28`, dumped live 2026-10-12): FrameBegin 0, ...,
16-20 (InGameStep, MoveMapStep), ..., 78, ..., 113, 117, 118 (MoveMap_PostPhysics_0), 120-122 (DmgMan), **123-125
(MoveMap_PostPhysics_1)**, 126 (MenuMan), ..., **106 CameraStep, 107 DrawParamUpdate**, ..., 142 ScaleformStep, 144 Draw_Pre,
..., 160, ..., 168.

### Control hand-over

The **puppet** starts when the guest is alive, attached and `kGuestReady`, and a validated player is
`Active` in the world (its ChrSetEntry `+8` load status: 3 while loading, **4** in the world, VERIFIED live for the
player and the NPCs around him; fromsoftware-rs's 2 never appeared, so a 15 s fallback used to delay Snake's start),
and no blocking screen (the loading screen) is up. While it is active:

- the Tarnished is hidden (`ChrIns+0x1C5` bit 3, or `hide_mode = fade`) -- his body and armour -- and his base
  transparency (`ChrIns+0x24C`, fromsoftware-rs `base_transparency`, 1 normally) is held at 0, which takes his weapon
  models with him (they are `CSPartsModelIns` under `PlayerIns+0x648` CSChrAsmModelIns; `fade`, the render group bit
  0x1C4 & 8, `max_render_range` +0x408 and the parts' display entity +0x20 left them visible: session 4's "a sword or
  spear at Snake's feet"). The value found at the start (1, or a sane 0..1 one; else 1) is written back when the puppet
  stops. VERIFIED live 2026-10-12 evening: the Tarnished and his longsword both gone, the world drawn as before;
  `puppet hide <mode>` switches live;
- the data module's NoDead bit is set;
- ER's pad is masked to Start/View (unless an ER menu is open);
- MGSV gets the real pad.

The puppet stops, and everything it changed is restored, the same frame any of these happens:

- the guest heartbeat times out;
- a load starts;
- the player dies;
- the player object changes;
- the console says `puppet off`.

Elden Ring has the controller back on that frame.

**Menus**: the primary signal is CSMenuMan+0x798 (non-zero while a blocking menu is open; re-backlog
E4, code-verified, coverage checked live; every change is logged as `ER blocking menu open/closed`).
`er/MenuWatch.cpp` stays as the fallback and OR'ed in: after a Start or View press, a latch (1.5 s)
treats an ER menu as open. Meanwhile CSMenuMan UI states that turn visible after
the press, and are still visible when the latch ends, are *learned* as menu states. The log says
`learned ER menu UI state 0x..`; pin those values in `[Input] menu_ui_states`. Entries learned or armed together form a
group; an entry also seen as Elden Ring's HUD (turned visible with no press and no menu open, or already up when the press
came: `HUD too` in `pr er status`) neither keeps its group open nor closes it (`MenuWatch::GroupOpen`, tested): a banner
that came up inside the latch no longer hands the pad back to MGSV inside a menu, and a boss bar no longer keeps the map's
group open. While an ER menu is open,
ER gets the whole pad and MGSV gets `kPadNeutral`. The same holds while an **ER cutscene** plays
(`[[CSRemo]+8]+0x14C` bit 0, E4; logged as `ER cutscene playing/over`, `kHostCutscene` set, puppet motion
paused).

**Interaction prompts** (re-backlog E6): while `CSActionButtonMan+0x20` holds the on-screen prompt
(logged with its ActionButtonParam id), a *fresh* press of `[Input] prompt_button` goes to Elden
Ring until released, and MGSV never sees that button while the prompt is up (a button already held for
MGSV never turns into an ER interaction). The button is **Y** (0x8000): Elden Ring's "event action" on an Xbox
pad (VERIFIED live: Y rests at a grace and acquires materials, A does nothing there). MGSV's own Y (mount D-Walker,
pick up) is held back while Elden Ring's prompt is up, so at a grace with the walker beside him Snake rests.

**Interactions** (`er/Interaction.h`, `[Input] interaction_*`): a prompt press hands Elden Ring the whole pad (MGSV
neutral, the puppet holds still and its odometer baseline follows) for `interaction_min_ms` (3.5 s: a pickup, the
rune retrieval), and for as long as Elden Ring's in-game HUD is hidden. That HUD is CSMenuMan UI state 7 (`03`
playing, `01` hidden): hidden 2.3-2.9 s after resting at a grace, while talking or reading, during loads and
cutscenes, in "Choose revival location". The grace menu is no "blocking" menu (CSMenuMan+0x798 stays 0) and not a
state Start/View learning finds, so before this it got the masked pad and could not be left. The HUD is used only when
it was up at the press and was seen visible since the last load: a hidden HUD is not a menu signal by itself (ER's
"auto" HUD setting may hide it while you just play -- UNVERIFIED which state that hides). Live: Y at the Gatefront
grace -> its menu, B -> Snake plays on; Y at a bloodstain -> runes back; a pickup -> Snake back after 3.5 s. Until the HUD has
hidden, Elden Ring gets only the face buttons, D-pad, Start and View (`InputBridge::SetButtonsOnly`; an Elden Ring menu
opened meanwhile gets everything): with the whole pad the stick, bumpers and triggers walked, rolled and swung the hidden
Tarnished for the rest of the 3.5 s after a 1 s pickup, and Snake then jumped there (UNVERIFIED live).

**View: Elden Ring's map or MGSV's iDroid** (`[Input] view_hold_ms`, 400): while Snake plays, a View *tap* still opens
Elden Ring's map, but Elden Ring only gets it once it is released (a 120 ms synthesized press), so a View *held* 400 ms
goes to MGSV instead (its iDroid) and Elden Ring never sees it (MenuWatch's Start/View latch only counts the presses
Elden Ring got). Logged as `input: View held 400 ms -> MGSV (its iDroid; N so far)`. VERIFIED live with fake_mgsv: tap ->
ER's map, MGSV no Back; 700 ms hold -> MGSV's Back, no ER menu.

**MGSV's own menu** (`[Input] guest_menu_owns_pad`): while GuestState `kGuestInMenu` is set (iDroid, pause) MGSV gets the
whole pad -- Start, View and Y included, to work the menu and close it -- and Elden Ring a neutral one (no map, no menu,
no prompt press); `MGSV menu open/closed (kGuestInMenu)` in the log. An Elden Ring menu that is already open keeps the pad
(MGSV had a neutral pad then). Elden Ring's own HUD stays as it is: MGSV's HUD layer (the iDroid is part of it) is drawn
over it, and hiding Elden Ring's HP/FP bars meanwhile would need its HUD state written (CSFeMan+0x78) for nothing a player
needs. VERIFIED live with `fake_mgsv !menu on`: Start, View and A reached MGSV (3 in-menu presses), Elden Ring opened
nothing.

**Button glyphs** (`er/KeyGuide.cpp`, `[Input] pad_glyph_device`): Elden Ring decides between pad and keyboard glyphs by
the device it last saw input from, and while Snake plays its pad sees only Start/View: "E: Acquire materials" on every
prompt (VERIFIED live, even with injected pad presses). The choice is one byte: the key-guide object at
`[eldenring+0x3D61F08]` (0x88C bytes, built by `0x242790`, which stores the clamped `Debug.KeyGuideDevice` config value
at +0 and +4) **+4: 0 pad (Xbox glyphs), 1 keyboard, 2 none** (VERIFIED live by poking it: the map's key guide went from
"E Q R F Z" to "A B X Y LB R"). While the puppet runs it is put back to `pad_glyph_device` (0) whenever Elden Ring changes
it (`key guide: Elden Ring chose glyph device 1 while Snake plays -> 0`); it is not restored afterwards (the player holds a
pad; Elden Ring switches it again on keyboard/mouse input). Live: "Y: Retrieve lost runes", "D-pad: Switch action".
Address table entries `key guide state` (the `mov [rip+x], rax` at 0xDEDF65) and `key guide ctor` (69/69 verified).

**MGSV's pad while Snake plays** gets everything but Start/View (Elden Ring's menu and map: one Start used to open
ER's menu and MGSV's iDroid together) and, while ER's prompt shows, the prompt button. Buttons and triggers already
held when MGSV gets its pad back (the A that confirmed "Choose revival location", the B that closed a menu) stay out
of its pad until released (one crouched Snake after the revival).

**Player controller**: `PlayerIns+0x58` is a `CS::PlayerCtrl` (its constructor 0x1403DDF10 is called from
the PlayerIns constructor 0x140650C90), not a plain `ChrCtrl`. `MainPlayer()` accepts PlayerCtrl,
ChrCtrl, or the root-motion vtable copy. (The first version checked for ChrCtrl only, which would have
rejected the real player.)

**Axes** (`er/Anchor.h`, `er/Calibration.cpp`): `ER = Pe + Ry(delta)·M(P_mgsv − Pm)`. `delta` is fixed
when the puppet starts (the puppet keeps its facing). `M` is the optional X mirror
(`[Axes] mirror_x`). Two Elden Ring conventions are *voted* from the game while the player still drives
the Tarnished:

- the character's forward axis (run direction vs `quat·(0,0,1)`);
- the camera `+0x30` row sign (camera → player vs that row), plus the row handedness.

The ini can pin each of them.

### Motion and hits: the two RE-backlog hooks (root motion the default since 2026-10-12; hit_hook on since phase 3)

Both are **instance-vtable swaps** (`er/VptrSwap.h`): the one game object gets a private copy of its
vtable with one slot replaced; no code or `.rdata` is written. Both are installed when the puppet starts
and restored when it stops (only on the same, live object).

- `[Puppet] motion = root_motion` (**the default**; re-backlog E1, `er/RootMotion.cpp`): PlayerCtrl vfunc 12
  (`0x3DE610`, verified against the vtable slot and the `call [rax+0x60]` site at `0x3CC85C`) runs the
  original, then replaces the root-motion translation with Snake's displacement (Havok world → character
  local with conj(phys+0x50)). ER's own proxy then moves the puppet with collision, slopes and gravity: Snake is pushed
  out of anything he walks into the way the Tarnished is, so he can't end up inside a cart or a rock (session 4's
  kinematic Snake walked into a wooden cart between its knee and chest rays and stayed stuck in it).
  The task only adds to an accumulator that the hook takes. If the hook stays silent for 1 s the puppet
  logs it and falls back to `kinematic`.
  **Catch-up** (`[Puppet] catch_up`, 0.6 m): live, ER's proxy moves 60-95% of what it is handed (less uphill), so Snake
  drifted behind MGSV's. The difference between what Snake *asked* (MGSV's displacement) and what the proxy moved is kept as a
  debt and handed back on later frames (gain 0.35, at most `catch_up` m owed); it is dropped (x0.8 a frame) while the proxy
  moves under 30% of what it was handed (a wall: Snake must stay blocked, not tunnel) and decays (x0.7) once Snake has
  stopped for 150 ms. (A first version counted *handed* minus moved and ran away: 601%, 12 m/s.) `pr er status`:
  `root motion, moved X of Y m asked, catch-up owed Z m`. VERIFIED live 2026-10-12 (fake_mgsv `!move`, Limgrave camp):
  99-109% of the asked distance on open ground per walk, 35.4 of 41.6 m on a walk ending against an ornate carriage
  (stopped at it, not in it); the ER camera and compass unchanged. `puppet motion kinematic|root_motion` switches live.
- `[Puppet] motion = kinematic` (the old way, `er/Kinematic.cpp`): our own capsule against `cs_phys_world_cast_ray`. It
  now probes walls at knee, waist (0.9 m) and head (1.6 m) height as well as the chest, and **unsticks**
  (`[Puppet] unstick`, `unstick_seconds` 0.5): every third frame `Enclosed()` counts which of 8 directions meet geometry
  within the body's radius + 0.3 m; boxed in (7-8) for `unstick_seconds`, the puppet goes back to the newest place it
  stood free (kept every 0.25 s, the last 2 s, at least 0.5 m away; `kinematic::Unstick`, tested). Its body bumping into
  something (a blocked step) also sends the object-only breaker there (below), at most every 400 ms. UNVERIFIED live
  (root motion is the default).
- `[Puppet] hit_hook = true` (re-backlog E2, `er/HitHook.cpp`): CSPlayerDamageModule vfunc 7
  `ReceiveDamage` (`0x44D050`) on `[ChrIns+0x190]+0x98`. While the puppet is active the original is
  skipped (no HP loss, no stagger) and the hit (amount `+0x228`, direction `-(+0x1C0)`, attacker
  distance → melee/projectile) becomes `kHEvHurt`. Not skipped once Snake has died (ER must run its
  death). Poison/fall damage still arrive through the HP-delta check.

### Breakables: pots, crates, barrels, carts (`er/Explosion.cpp` Break/Shrapnel, `er/GeomProbe.cpp`, `[Combat] break_objects`)

Elden Ring breaks a destructible asset (a `CSWorldGeomIns` whose AssetEnvironmentGeometryParam row has hp > 0) when an
attack with enough `atk_obj` (AtkParam +0x62) reaches it, or, for the assets with param bits_3b bit 0, when the
player's body runs into it. The Tarnished is hidden and still, so Snake does it with bullets of his own:

- **What breaks things in Elden Ring**: its flying Fire Pot (Bullet 10030000) carries AtkParam_Pc 10030000 -- no damage of any
  kind, atk_obj 10 -- and breaks a cart it flies into (VERIFIED live with `boom raw 10030000 x y z`: AEG003_057_1031 down).
  Its burst (10030001) spawned *standing* at a point breaks nothing: an object is hit along a bullet's flight.
- **The breaker** (look `explosion_breaker`, carrier Bullet 10302003): a copy of the flying pot with no flight/hit/flick
  effect, no sound, no children, hit radius `breaker_radius` 0.15 m, flying `breaker_speed` 25 m/s from `breaker_lead`
  0.6 m before the point to as far past it, carrying `breaker_atk` (AtkParam_Pc 10048000, the cut Roped Freezing Pot's,
  patched by `explosion::MakeObjectOnlyAttack`: phys/mag/fire/thunder/dark/stamina/throw 0, SpEffects -1, poise 0,
  knock-back 0, dmg level 0, atk_obj 10). Sent through the point where Snake's round hit the world (not tranquilizer or
  no-damage rounds, not D-Walker's flame), where his CQC strike met no one, and (kinematic) where his body bumped into
  something; at most `breaker_per_second` 20.
- **Shrapnel** (look `explosion_shrapnel`, carrier 10302004, `explosion_atk` AtkParam_Pc 10048001 at atk_obj 100): every
  lethal blast (rockets, grenades, C4, mines) sends `shrapnel_count` 8 of them out level (and one down) from 0.4 m above
  its centre to its radius.
- `boom set breaker_speed|breaker_radius|breaker_lead|breaker_obj|explosion_obj <v> | hits_characters on|off`, `boom raw
  <bullet> x y z` (an unmodified Elden Ring bullet from the Tarnished's chest, to see what Elden Ring itself breaks).

**Asset probe** (`er/GeomProbe.cpp`, console `geom`): CSWorldGeomMan (FD4 singleton) `+0x18` the loaded blocks (an MSVC
`std::map`: allocator, head `+0x20`, size `+0x28`; node left 0, parent 8, right 0x10, isnil 0x19, value pointer 0x28);
each block's instances: vector `+0x290/+0x298`, array count `+0x2C8`, array `+0x2D0` (stride 0x430); a `CSWorldGeomIns`:
param row `+0x20` (hp i16 `+0x10`, defence u16 `+0x12`, bits_3b `+0x3B`), MsbPart `+0x48` (its name, PCWSTR, at +0),
world matrix rows `+0x220..`, **translation `+0x250`** (VERIFIED live: the matrix rows are orthonormal and the
translation matches where the asset stands; `geom learn` votes it again). `geom near|all [radius [filter|-filter]]`,
`geom dump <ins>`. Every pointer read goes through SafeRead.

VERIFIED live 2026-10-12 (Limgrave camp south of the Gatefront ruins, fake_mgsv; "broken" = vertical ray probes over the
asset's centre drop from its top to the ground, `breaktest.py` in the session scratchpad, plus screenshots):
- a test rifle round (`combat shoot rifle x y z`): `Snake rifle ... -> world at (22.15, -3.72, 7.17)` -> `object-only
  breaker through (22.15, -3.72, 7.17)` -> wooden cart AEG003_057_1028 top 0.96-1.72 m -> ground level;
- a CQC strike at no one (`combat strike`): carts AEG003_057_1032 (top 1.46-1.67 m -> 0.12) and AEG003_069_1017 down;
- a rocket (`combat shoot rocket`, flown, shrapnel): crate AEG003_058_1028 top 0.73-1.03 m -> 0.11-0.14 (5.5 m flight);
- 4 of the 17 probe runs broke their target; most of the rest were shots from 15-29 m that met a tent or the terrain
  first (the breaker goes where the round hit). Low crates AEG003_054_1030/1034 (0.77 m, defence 0) stayed up under
  close strikes: UNVERIFIED why (their collision may sit below the breaker's path).
- **not broken**: a defence-90 Limgrave fence (AEG700_013_1037, bits_3b 0x40) by rocket shrapnel at atk_obj 100 *and*
  999 (`boom set explosion_obj 999`): defence-90/95 assets need something else (UNVERIFIED what: an attack type, a
  behaviour); AEG003_058_1025 (defence 0) broke neither for our breaker nor for Elden Ring's own flying pot.
- **Touch-breakable pots** (bits_3b bit 0, e.g. AEG430): none were loaded near the test spot. With root motion Elden
  Ring's proxy moves the Tarnished's body into them; whether that counts as the player's body breaking them is UNVERIFIED.

### Explosive rounds in flight (`er/Rounds.cpp`, `[Combat] round_flight`, `round_speed_rocket` 20, `round_speed_grenade` 80)

MGSV flies its rockets over its own airport and tells us only the shot; the host used to put the blast at Elden Ring's
impact point the moment the shot left the gun, often 30-70 m away, so the player never saw it go off (session 4). Now a
rocket (TppEquip `BL_ms*` 43-60, 35, 75, 84, 85, 245) or a grenade round (23, 27, 31, 63, 64, 96, 103; `Rounds::FlightOf`)
flies from Snake's muzzle at its speed -- measured from session 4's guest log: rocket blasts 0.87-1.08 s after the shot
at 16-22 m (~20 m/s), the ZORN-KP's ~100 m/s -- one segment per frame against Elden Ring's world (`CastRay`) and the
hostile capsules (`Combat::HostilesNear`), and goes off at the first hit or after `trace_range` (150 m) / 15 s: the old blast
(`Combat::BlastAt`: damage, Elden Ring's fireball, shrapnel), 0.3 m back along its path. Drawn with the world markers: a
small dark body (8 cm), a bright flame (0.32 m) at its tail, a glowing exhaust and the grey smoke of the last 0.6 s every 0.4 m.
Moved with Elden Ring's origin shifts; dropped when the puppet stops.

**Rocket motor** (`Rounds::SpeedAt`, `[Combat] round_speed_rocket` 16, `round_speed_rocket_boost` 60,
`round_rocket_boost_after` 1): MGSV's rockets leave the launcher slowly and their motor takes over -- its own blasts came
0.9-1.1 s after the shot 16-22 m away but 3.3-3.5 s after it 150 m away, which no single speed fits (20 m/s made a 150 m
shot take 7.5 s). A rocket flies at 16 m/s for 1 s, then ramps to 60 m/s over 0.3 s; grenade rounds keep
`round_speed_grenade` 80. VERIFIED live with both games (2026-10-12 evening, FB MR R-LAUNCHER): `went off (hit the world)
... after 0.55 s, 8.6 m`, `after 3.17 s, 138.9 m`, `(end of its range) ... after 3.37 s, 151.0 m`; screenshots at 20 and
50 m show the rocket and its trail in flight.

VERIFIED live 2026-10-12 (fake_mgsv `!fire rocket`, before the motor, at 20 m/s): `rounds: ... launched at 20 m/s` -> `went off (hit the world) at
(-16.38, 3.58, -8.27) after 0.87 s, 17.3 m`, `... after 2.66 s, 53.2 m`, `(hit an enemy) ... after 0.12 s, 2.3 m`; screenshots
at 20 and 50 m show the comet with its trail in flight and the fireball cluster where it hit.

### Snake's cardboard box (`er/Stealth.cpp`, `[Stealth]`, protocol v8 `kGuestInBox` / `kGuestBoxPoster`)

The least invasive way to make Elden Ring's enemies not see the Tarnished is Elden Ring's own stealth: a SpEffect on him
with `sight_search_enemy_rate` and `hearing_search_enemy_rate` 0 (Unseen Form and Assassin's Approach work like this).
`[Stealth] speffect` 20004999 is such a row of the regulation (no visual, no end time). While Snake is in MGSV's box and
keeps still (under `still_speed` 0.4 m/s), or creeps (under `slow_speed` 1.8 m/s) for `slow_grace` 1.5 s after he last
kept still, it is applied (`ChrIns::ApplySpEffect`, the address the combat code already uses); a hostile within
`reveal_distance` 2 m, moving faster or leaving the box takes it off (`RemoveSpEffect`); the active list (ChrIns `+0x178` ->
`+8` head, entry `+8` id, `+0x30` next) shows whether it is on. Off when the puppet stops or Snake dies.
`Stealth::BoxHidden` is pure and tested.

With a poster on the box (`kGuestBoxPoster`), a hostile within `poster_range` 15 m whose eyes (top of its capsule) see the
box (70° in front of it, a clear ray) stops and stares for `poster_stare` 4 s: its ChrIns debug flags no-attack/no-move
(`+0x538` bits 4/5, those of the console `flag` command), only the bits we set are cleared again; `poster_cooldown` 12 s
before the same one stares again.

VERIFIED live 2026-10-12 (fake_mgsv `!box on`): an idle Limgrave soldier 5 m from the still box ignored it for 9 s (the hit
hook's count stayed at 18); without the box he attacked within 3 s (18 -> 23); `stealth: Snake seen again (moving)` when
he moved. **Limitation**: a soldier who had already seen Snake *before* he got in walked up to the box (2 m: revealed) and
attacked: Elden Ring's AI keeps chasing a target it has (its search slots in CSTargetSearchSys are not touched).

VERIFIED live with both games (2026-10-12 evening): `stealth: Snake hidden in the box (keeping still) -- SpEffect
20004999 on the Tarnished: active` / `(creeping)` / `Snake seen again (out of the box) -- SpEffect 20004999 taken off`;
the poster stare six times: `stealth: npc 60100010 team 6 hp 55/55 sees the poster box 14.1 m away and stops to stare
(4 s)` (11.4-14.8 m) and `npc 43111110 team 48 ... 7.2 m away`. A Gatefront soldier 6 m away left the still box alone for
10 s (hit hook count unchanged), but he also ignored Snake outside it in the control, so the fake_mgsv run above is still
the proof that the effect hides him.

**The poster drawn** (`[Render] poster_materials` c94150c9, shader flag `kShPoster`): MGSV draws the box's poster as a
layer -- texture 4 (`g_tex_layer`) sampled with the mesh's *second* UV set (TEXCOORD1, decoded like the first; a mesh
without one uses the first). The host blends it over the cardboard wherever the picture isn't its plain near-white
background (`cover = saturate((max |poster - (0.953, 0.949, 0.953)| - 0.012) * 25)`, sRGB-decoded when the texture is).
VERIFIED live: the pin-up on the box's lid, lit by Elden Ring.

### Elden Ring's bullet pool (`er/BulletSpawn.h`, `er/Explosion.cpp` Room())

CSBulletManager keeps the live bullets in a list (head `+0x08`, CSBulletIns next `+0xE18`) and counts emitters `+0x1D0` (in
its buffer) / `+0x1D4` (of 64 + 192) and bullets `+0x1D8` / `+0x1DC` (`boom pool`). **Emitter leak**, fixed: every
`SpawnBullet` (0x1403A2CB0 -> 0x1403A5A20) allocates an emitter, and the emitter (tick 0x1403900C0) finishes only when the
spawn data's flag `+0x44` bit 0 (copied by 0x14038E550 to the params `+0x194`, emitter `+0x4C4`) marks it one-shot -- else it
waits for more bullets for ever. Ours carried 0, so emitters piled up until Elden Ring refused every spawn (`error 4`).
`BulletSpawn::Fill` now writes 1 (one-shot), for hit bullets too: live, emitters 14 -> 7 -> 0 as the bursts ended.
`Room()` also caps what we spawn by Elden Ring's own counts: `explosion_max_live` 48 of ours, 192 emitters, 176 listed
bullets (a Fire Pot burst adds ~18 sparks: 7 bursts = 133 bullets). Clouds (sleep mist, smoke) last
`explosion_cloud_life` 6 s and are renewed only when their bursts are gone. Live: a stress of 250 blasts was capped
(no `refused`), emitters back to 0 after.
(Session 4's smoke "jumping" in the log was an Elden Ring world origin shift: the same place in new coordinates; the
cloud gate's spots now move with shifts, `CloudGate::Shift`.)

**When Elden Ring refuses anyway** (`out == -1`): a run of refusals is a `[warn]` -- the first one, then every 10 s, outside
the per-blast log budget (`Elden Ring refused SpawnBullet(...) -- N in a row, M in all; ... clouds wait until a spawn goes
through again`), and `takes our spawns again (after N refused)` when one goes through. While refusals are recent (10 s)
a cloud is not refreshed (session 4: a smoke cloud re-blasted 7 bursts every 2.5 s into a pool that refused them for a
minute, and the smoke was gone all that time); a blast's own first burst still tries. `boom` shows `N refused (N in a row
now)` and `cloud refreshes held back after a refusal`. The hit bullets (`er/Combat.cpp`) warn the same way (`... refused
by Elden Ring, N in a row NOW` in `combat`). Not seen live since the emitter fix: 0 refusals in the evening pass.

### Hidden Tarnished's weapon (fixed)

The Tarnished's render flag hides his body and armour, not his weapons (`CSPartsModelIns` in the CSChrAsmModelIns at
`PlayerIns+0x648`). His base transparency (`ChrIns+0x24C`) at 0 hides both: see "Control hand-over".

### Battle buddy (D-Walker first)

`er/Buddy.cpp` uses the protocol v2 GuestState buddy block:

- It logs `buddyType`, deploy/missing/ridden transitions, and D-Walker not developed (`kBuddyMissing`).
- It places the buddy from `buddyPos`/`buddyYaw`. When that is not valid, it uses the centroid of the guest's `kOwnerBuddy` draws (phase 2).
- It maps the buddy through the anchor and snaps it to ER ground with a ray. `GroundOffset()` is for the phase 4 renderer.
- It draws it (cyan) with CSEzDraw.

Riding (`kGuestOnVehicle` or `kBuddyRidden`) switches the puppet to `max_speed_vehicle`,
`step_up_vehicle` and `eye_height_vehicle`, so the puppet follows D-Walker's dash and hover through ER
collision. `kGEvBuddyFire` is ray-traced like Snake's own shots: it is logged, drawn red and names the
character it would hit. Damage: docs/COMBAT.md.

**D-Walker's orders made visible** (`er/BuddyOrders.*`, protocol v7 `kGEvBuddyOrder`). Player report: "it does not react
to my commands and it's not clear they are given or not" -- the sessions had no order at all. MGSV's call menu takes an
order only when the right stick is clicked *with the stick let go* (live: 2 of 2 clicks with the stick still pushed did
nothing, 4 of 4 after releasing it gave the order) while LB is still held. Now:
- every order MGSV took is in the corner ("D-Walker: Search Mode -- it marks enemies nearby"; live: `buddy: the player's
  order 25 -- ...` 6 ms after the guest saw MGSV take it);
- an LB hold of 0.6 s with the walker out and Snake on foot shows how ("keep LB held, point the right stick at one, let the
  stick go, then click it in"), until two orders were given;
- a hold that pointed at the walker's entries (stick right) and ended with no order says "No order given -- let the right
  stick go, then click it in while LB is still held" (live: both hints seen, `buddy` counts them).
The pad is the one the bridge sees before any mask; nothing changes what MGSV or Elden Ring get. Pure rules: `Hint`
(`tests/test_host.cpp`).

### Camera and Elden Ring's HUD compass (`er/CameraDriver.cpp`, `[Camera] sync_chr_cam`, `sync_compass_cam`)

Player report: "the compass is not steady at all". MGSV's camera goes into `pers_cam_1` (what Elden Ring renders from)
and `chr_cam` (its player camera: which far enemies animate every frame, 3D sound) at DrawParamUpdate (107); the compass
swirled anyway. What it really reads (Ghidra + live, ER 1.17.1):

- The compass meter is `PlayerHUD/PersonalCompass` (`0x1408D06E6`): `0x8D4530` sets its frame from CSFeMan's HUD data
  (`CSFeManImp+0x80`) `+0xFF0+4`, i.e. **CSFeMan+0x1074 = the heading in degrees** (`(atan2(-fwd.x, -fwd.z) - 90) mod
  360`), and places its markers from the lists after it.
- `0x7774A0` writes that heading from a camera's `+0x30` (forward) row; its only caller `0xB01FC0` passes
  `[[MoveMapStep+0xE8]+0x18]`: **GameRend's first CSPersCam** (GameRend = `FieldArea+0x20` = `CSFeMan+0x10`, vtable
  `CS::GameRend` 0x2A82338), refreshed just before by `0xB03AA0` -> `0x681970` from `MoveMapStep+0xF8` = **chr_cam**.
- That MoveMapStep task (`+0x378`, `0xB00B40`) is registered in **GameFlowInGame_MoveMap_PostPhysics_1_Core0 (123)**,
  which runs *before* CameraStep/DrawParamUpdate in the frame, and Elden Ring refreshes `chr_cam` from its own (spinning)
  follow camera between MoveMap_PostPhysics_0 (118) and DmgMan_Pre (120). So our 107 write never reached the compass.
- Fix: `CameraDriver::ApplyCompass()` at **DmgMan_Post (122)** writes MGSV's camera (this frame's, as last mapped) into
  `chr_cam` again. VERIFIED live (fake_mgsv, camera orbiting 0.25 rad/s, then still): CSFeMan+0x1074 stayed 270 (Elden
  Ring's own camera) with chr_cam / GameRend's camera / pers_cam_2 written at 107, at 10/16/18/20, or at 113/118; with chr_cam
  written at 120 or 122 it follows: still camera 214.4 deg on every sample (its forward (0.818, -0.132, -0.560) gives exactly
  that), turning 341 -> 358 -> 15 -> 31 deg at ~1.15 s steps. `hud shot` crops: the E and S glyphs steady while still, the W
  glyph crossing the meter while turning (scratchpad compass/compass_strip.png).
- Console: `pr er "peek [s.CSFeMan]+0x1074 f32"` (the heading), symbol `compasscam`, `cam set sync_compass 0|1`.

### Frame pacing: MGSV's motion shown evenly (`er/Pacing*`, `er/PacingProbe*`, `er/CameraCollision*`, `[Camera] smooth*`, `[Render] smooth_draws`)

Player report: "the camera is and has always been a bit jumpy". Measured first (`pacing probe`, below; both games, default
mapping, Limgrave forest and a path south of it, 2026-10-12):

- **Elden Ring** runs a steady 60 Hz: FrameBegin every 16.63-16.70 ms (p5-p95), two ~40 ms hitches per 10 s.
- **MGSV**, hidden in gamescope, runs **51-57 frames/s** (17.7-19.4 ms mean, sd 1.6-1.9 ms; 62 when Elden Ring isn't
  running) and advances its game by **one fixed step per frame** (docs/GUEST.md 19): a constant pan turned the camera exactly
  0.288 deg per MGSV frame whatever the frame took. So 12-15% of Elden Ring's frames showed the last MGSV frame again and the
  next one jumped two; Snake's draw lists trail his state by one MGSV frame (90%).
- What Elden Ring rendered (from `pacing probe` CSVs; jitter = each frame's distance from a quadratic fitted over the +-6
  frames around it, in FrameBegin time: smooth pans, runs, starts and stops fit, repeated or skipped frames and pops don't): panning at 17 deg/s the camera's yaw jittered **0.081 deg rms = 5.2 px at 4K** (p95 9.0 px, max 16.6 px);
  running at 4.7 m/s Snake, the camera and the world jittered **2.4-3.2 cm rms** (p95 4.6-5.7 cm).
- **Not** a source: the render thread pairs each frame with its own snapshot (Elden Ring's view-constant origin equals the
  snapshot's camera to 0.00 cm on 600/600 checks), Present - FrameBegin 8.8 +- 0.6 ms.
- Also jumpy: **camera collision** snapped the camera in and out (the eye-to-camera distance jumped up to 5 m in one frame,
  56 of 900 frames over 10 cm, panning next to a tree), and **MGSV's Lua Warp** (the treadmill) turned MGSV's camera behind
  Snake on every jump: running toward the camera it swung 180 deg every ~1.3 s (docs/GUEST.md 19; the guest now holds him
  with the native placement).

**What the host does now.** Each Elden Ring frame shows one moment of MGSV's past, blended from the two MGSV frames around it
(`er/PacingLogic.h`):

- **Timeline.** MGSV frames are placed on a step timeline (one step per frame, more when a frame took two nominal steps'
  time: MGSV caught up), stamped with their QPC (both games share the clock). The pace is MGSV's measured ms per step.
- **Clock.** Each frame Elden Ring's point moves by its own frame time at MGSV's pace, pulled at most 0.03 steps per frame
  toward "MGSV now" minus a delay; the delay is the 97th percentile of how far behind "now" the newest MGSV frame is
  (over 2 s) + 0.12 steps, so the frame after the point is almost always there (live: **1.13-1.27 steps = ~20 ms**; it
  was 9.8 ms on average before: +10 ms). 1.5 steps past the newest are carried on (an MGSV hitch), a 2.5-step disagreement
  (a pause, a load) jumps.
- **What is blended.** Snake's odometer (warp-free), his yaw, the camera relative to his root (continuous across the
  guest's treadmill jumps), its rotation (slerp) and fov; the puppet moves by the blended odometer (`er/Puppet.cpp`), the
  camera is the blended camera at the puppet's feet (`er/CameraDriver.cpp`) -- once per frame at FrameBegin, so both show
  the same moment. A cut (odometer 3 m, camera 1.5 m or 40 deg in one MGSV frame) is never blended.
- **Running.** Snake's root moves in whole animation keys, 2 or 3 per MGSV frame (7.8 / 11.7 cm, `2,2,2,2,2,3,3`),
  and MGSV's camera with it; `smooth_motion` (7) takes the puppet's odometer from a straight-line fit over the last 7 MGSV
  frames instead (recorded runs: 1.2-1.4 -> 0.5-0.7 cm rms, at most a few cm off MGSV's path in starts and turns, no
  overshoot at a stop).
- **Poses.** The draw lists are kept (the last 4) and blended per draw: same mesh range and owner, each 3x4 matrix's
  translation lerped, rotation slerped, scale lerped; at a point that trails the camera's by how late the lists come (its own
  adaptive delay, live 0.6-0.9 steps; a list published since FrameBegin is acquired at Draw_Pre).
- **Camera collision** (`er/CameraCollision.h`): four more rays 0.15 m around the camera, the pull-in at once, the way
  back out eased (0.2 s); the pivot no higher than the camera (crouched, prone).
- **Vertical:** the camera follows the kinematic puppet's feet height eased (0.06 s): ground snap on a step or a rock no
  longer hops the camera.

**Measured after** (same places, back to back; VERIFIED live 2026-10-12, both games, default mapping):

| | before | after |
|---|---|---|
| host blending alone (`pacing set smooth off/on`, MGSV unchanged): pan, camera yaw jitter | 5.2 px rms, p95 9.0, max 16.6 | **0.59 px rms**, p95 0.53, max 7.1 (an Elden Ring hitch) |
| everything (MGSV's own Sleep(1) and no blending vs the shipped defaults): pan | 3.5 px rms, p95 7.6, max 13.5 | **1.05 px rms**, p95 1.4, max 9.3 (a 54 ms MGSV hitch) |
| running forward / toward the camera: Snake, camera and world jitter | 2.9 / 3.0 cm rms, p95 5.0 / 5.8 | **0.33 / 0.73 cm rms**, p95 0.6 / 1.6 |
| Elden Ring frames showing no new MGSV frame (steady parts) | 6-14% | 0 (blended) |
| MGSV frames/s panning / running (docs/GUEST.md 19) | 54 / 52 | 57.5 / 57-59 |
| collision: eye-to-camera distance change per frame | up to 5 m | at most 0.28 m (bushes, 39-56 frames pulled in) |

Elden Ring held 59.4 presents/s; render CPU 0.15 ms per frame (render thread) + the draw blend 0.07 ms (task thread).
Re-measured in the combined integration test (same method, `pacing probe` + the scratchpad's `jitter.py`; jitter of
frames more than 6 frames from one of Elden Ring's own >20 ms frames): pan 0.15 px rms (p95 0.04, max 2.2), all frames 2.9
px rms (6 Elden Ring hitches of 36-39 ms in 9 s on the desktop, 1 in 9 s with Elden Ring in headless gamescope); running
forward / toward the camera 0.40 / 0.44 px yaw, 0.54 / 0.48 cm camera position rms; riding D-Walker 1.2 px, 0.23 deg
largest per-frame yaw step once the riding camera is kept (docs/GUEST.md 19; 101 deg jumps before).
Screenshots while running and turning: Snake's blended poses look as before (no stretched limbs).

**Console.** `pacing` (state: ms per MGSV step, delays, blended / extrapolated / held frames, snaps, cost), `pacing set
smooth|draws on|off`, `pacing set delay auto|<steps>`, `pacing set extrapolate <steps>`, `pacing set motion <frames>`, `cam
set collision_radius|collision_ease_out|vertical_smoothing <v>`. **Measurements:** `pacing probe start [frames]` records
per frame what FrameBegin read (MGSV frame, its stamp, odometer, root, camera), the draw list, the puppet's move, the camera
written at DrawParamUpdate and its collision ray, the snapshot taken at Draw_Pre, when the render thread used it
(G-buffer, Present, the view-constant check, every frame while recording) and the paced step and blend; `pacing probe
dump [file]` writes it as CSV next to the DLL.

### Thrown and placed explosives, world markers (protocol v5/v6: `er/Throw*`, `er/Mines*`, `er/RenderMarkers*`, `[Throw]`, `[Mines]`)

Player reports: "I can't see the trajectory or where the grenade lands", "sleep grenade did not work at all, neither did
mines". MGSV flies its grenades over its own flat airport, and its proximity mines only ever trigger on its own characters
(there are none). The host now keeps its own copies in Elden Ring's world:

- **Throws** (`kGEvThrow`): MGSV's release point and velocity, mapped through the anchor at the release, flown with MGSV's
  gravity (9.8) through Elden Ring's collision (`cs_phys_world_cast_ray`, one ray per 1/60 s; at a hit two parallel rays give
  the surface normal): bounces (restitution 0.3, friction 0.65), rolls (3 m/s^2) and lies still (`er/ThrowLogic.h`, pure,
  `tests/test_host_throw.inc`). Drawn as a small lit olive ball. The matching `kGEvExplosion` (`pad[1]` = throw id) puts the
  blast -- damage and Elden Ring's pot effect (`Combat::BlastAt`) -- where our copy is. Fused throws (pad[0] >> 16: MGSV's
  frag and sleep grenades: 3000 ms) go off at MGSV's report; impact ones (fuse 0) where our copy first touches the world
  (radius from MGSV's earlier report of that equip id, else `[Throw] radius_*`), MGSV's report then only refreshes a cloud.
  A cloud's later reports stay on our copy. A copy that falls out of the world (or 20 s without a report) is dropped and MGSV's
  own point through the anchor is used.
- **Aiming arc** (GuestState `kGuestThrowAim`, v6 `aimThrowPos/Vel/Kind`): traced through Elden Ring's collision each frame
  (chords of 1/30 s), drawn as a thin line of bright pale-blue dots 0.3 m apart (radius 2.5 cm) and a ring (0.55 m) with a
  centre dot where it lands, on the surface's slope. `throw arc on` draws one from the camera for tests.
- **Placed** (`kGEvPlaced`): kept where it was placed (anchor at that moment, set down on Elden Ring's ground with its slope),
  drawn as a small dark body with a blinking light (red lethal, blue sleep, amber stun; steady while arming 1.5 s and once set
  off). Every third frame, a character the Tarnished may attack (`Combat::HostileWithin`: Elden Ring's team relation, capsule
  distance) within MGSV's trigger radius sends **`kHEvDetonate`** once; MGSV sets it off and its blast (pad[1] = placed id) is
  put where our mine lies. C4 (trigger 0) only from MGSV. `kGEvPlacedGone` drops it. Console `mines detonate <id>`.
- **Markers** (`er/RenderMarkers.h`): camera-facing dots and lit balls, rings lying on a surface, built on the CPU each frame
  (owners set them at FrameBegin, `RenderScene::Capture` copies them), drawn with one premultiplied-alpha pipeline into Elden
  Ring's tone-mapped scene colour right before its composite (the HUD stays on top), depth-tested in the pixel shader against
  its scene depth with a per-marker slack (rings and mines 0.35-0.4 m: grass must not hide them; walls do), else at Present.
  None while an Elden Ring menu (as detected), cutscene, load, death or MGSV's menu is up.
- Mines are **kept** when the puppet stops (an MGSV stall, a menu): the guest announces every live mine again on each new
  start of the puppet (HostState kHostPuppeting going up) with `kPlacedAgain` (protocol v9, `kGEvPlaced` pad[0] bit 16),
  and a mine we know stays where it lies (before, it was dropped and re-placed through the new anchor: next to wherever
  Snake started again). If Elden Ring loaded meanwhile (a death's respawn, fast travel: `loading`, or another player
  object), its world moved under them: at the next start each is **discarded** -- `kHEvDetonate` makes MGSV set it off out
  of the way (the guest skips MGSV's own blast) and its blasts are ignored here. A re-announced mine we never knew
  (placed for an earlier Elden Ring session) is discarded the same way; a new guest (another attach) forgets them all.
  `er/WorldShift.cpp` moves stored positions with Elden Ring's origin shifts. VERIFIED live with both games (2026-10-12
  evening): `mines: puppet stopped; 1 mine(s) kept where they lie` -> `mine 1 announced again -- kept where it lies` ->
  set off there; after a death: `mine 2 was placed before a load (a death, fast travel) ... -- discarded` and 8 x `blast of
  discarded mine 2 ignored`.
- A set-off that brings no blast within 3 s (MGSV refused it) is cleared and sent again by the next enemy in range, at most
  3 times (`throwing::DetonateRetryDue`). One character scan per scan frame covers every armed mine (`throwing::Enclosing`,
  `Combat::HostilesNear`), not one per mine.

VERIFIED live 2026-10-12 (ER 1.17.1, Gatefront, `/dev/shm/phantomring_h`):
- **Real MGSV recordings** (`pr replay throw_frag.prrec`, `throw_sleep.prrec`, `place_sleepmine.prrec`, guest commit 2ddb998):
  the aiming arc and its landing ring over the grass from MGSV's over-the-shoulder aim camera; `throw 1: lethal (equip 354,
  fuse 3000 ms) released at ER (2.84, 3.00, 3.99) at 16.7 m/s` -> our copy bounced 4 times up the slope and lay still ->
  `lethal blast r 7.0 m at (-16.51, 6.77, 17.66) -- MGSV's fuse (3.02 s after the release)`: Fire Pot fireballs on the
  hillside; the sleep grenade (fuse 3000, MGSV's first report 4.99 s after the release) misted where our copy rolled, its
  cloud reports followed the canister; the sleep mine's blast and 4 cloud reports went off where it lay.
- **fake_mgsv** (`!throw`, `!aimthrow`, `!place`, `!detonate`): frag 15 m away after 5 bounces; stun grenade flash and smoke
  cloud where our copies stopped; a Gatefront soldier walking up to the puppet came within 2 m of a placed mine:
  `mines: mine 2 ... set off: npc 43114010 ... at 2.0 m -> kHEvDetonate`, MGSV's blast back 17 ms later, put on the mine,
  the soldier killed (ER 39 + 224 topped up); C4 only from `!detonate`.
- Cost: 0.054 ms per frame while aiming (arc), 0.014 ms per flying grenade.

VERIFIED live with both games (2026-10-12 afternoon, default mapping, Gatefront and Agheel Lake, the controller driven
through `input virtualpad` + `press`):
- the arc while LT aims a sleep grenade (screenshots: the dots over the grass, rising over a sleeping enemy); `throw 1:
  sleep (equip 378, fuse 3000 ms) released ... at 14.2 m/s` -> `sleep blast r 9.0 m ... MGSV's fuse (5.00 s after the
  release, 5 bounce(s), rolling)` with 7 Sleep Pot mists renewed every 2.5 s;
- sleep gas over a group: three Limgrave soldiers of two resistances (169, 197) took the same +39 per puff, 66 build-ups,
  `68 sleep (0 lost)` (before the fix the second kind's were "LOST (another bullet holds the row)"), three of them slept
  (sleep gauge `+0x64` 1-2, thresholds raised 169 -> 197, 197 -> 239);
- a sleep-gas mine placed 0.8 m from a Gatefront soldier: `set off: npc 43114010 ... at 0.8 m -> kHEvDetonate` 1.5 s after
  it was placed, MGSV's blast back 0.7 s later where it lies, the cloud's puffs on him; a mine placed earlier was set off by
  MGSV itself in its world when Snake walked away (kept out by the guest, docs/GUEST.md 18.8);
- the rocket launcher (bullet 59) and the ZORN-KP (bullet 23): a Fire Pot fireball cluster at the impact (screenshot), the
  soldier hit by the grenade round killed (`hp 219 -> 214, +371 topped up`).

### Events

- **In:**
  - `kGEvFire` and `kGEvBuddyFire`: traced, resolved against character capsules, damage/poise/sleep through ER's own pipeline (`er/Combat.cpp`, docs/COMBAT.md).
  - `kGEvMelee`: the nearest hostile in the strike sphere or the arc in front of Snake takes the hit (docs/COMBAT.md).
  - `kGEvDied`: drops NoDead/NoDamage and calls `ChrIns::Kill(chr, false)` (`kill_on_guest_death`), so ER runs YOU DIED and the grace respawn.
  - `kGEvRumble`: sent to the real pad through the original XInputSetState.
  - `kGEvExplosion`: blast damage around the mapped centre -- or, with `pad[1]` = one of our throws / mines, where our copy is
    (above). `kGEvLog`: logged.
  - `kGEvThrow`, `kGEvPlaced`, `kGEvPlacedGone` (v5): `er/Throw.cpp`, `er/Mines.cpp`.
- **Out:**
  - `kHEvHurt`: HP loss on the puppet, sent as the share of the Tarnished's max HP it took, in units of Snake's life / guest life scale (`[Combat] hurt_share`, docs/COMBAT.md; the hit hook measures the hit at full HP). `v0` is the unit direction from Snake toward the attacker (`last_hit_by`), in MGSV world. The kind is melee if the attacker is under 4 m away, projectile if further, fall after a hard landing, and other when the attacker is unknown.
  - `kHEvRespawn`: dead → alive.
  - `kHEvWarped`: ER moved the player more than `resync_distance` by itself.
  - `kHEvDetonate` (v5): an Elden Ring enemy within a placed explosive's trigger radius (`b` = placed id), once.
- **HP bar = Snake's life** (`mirror_life`): every frame HP = max(1, round(max HP × life / lifeMax)) (`combat::MirrorHp`); while
  MGSV loads, the last value stays. The hit hook measures Elden Ring's hit at full HP and puts the *mirrored* HP back
  (er/HitHook.h); NoDead keeps the last point; Snake's death still drives Elden Ring's (`kGEvDied` -> `ChrIns::Kill`).
  NoDamage stays off by default, because hits are detected by HP loss. VERIFIED live (fake_mgsv, Gatefront soldiers):
  `!life 3000` -> HP 261/522 and the bar at half; a soldier's hit -> Snake 6000 -> 4621, HP 399 (76%), then the bar climbs
  with MGSV's regeneration; life 1 -> `Snake died` -> `ChrIns::Kill` -> YOU DIED -> grace respawn. At 7% Elden Ring shows
  no low-HP screen effect (screenshot); its HP-dependent talismans (e.g. low-HP attack boosts) see Snake's share, which only
  changes Elden Ring's own part of our bullet hits (the top-up completes the wanted damage either way).
- **Heals** (`forward_heals`): HP above the mirror set last frame is an Elden Ring heal (Crimson Tears, resting at a grace,
  a regenerating talisman): `kHEvHeal` (protocol v3) carries the same share of Snake's life (`combat::HealForGuest`, small
  regeneration ticks carried over), the guest adds it, the mirror shows it. Not while max HP just changed (a talisman
  swap may rescale HP). VERIFIED with fake_mgsv: +139 HP (27%) -> `kHEvHeal 399` -> Snake +1596 of 6000. The real guest
  ignores `kHEvHeal` until mgsv/Frame.cpp handles it (then a heal is put back by the mirror: effectively blocked).
  Flasks themselves can't be drunk while Snake plays (Elden Ring's item button is MGSV's), so heals come from graces and
  effects.

### Dev console (`er/Console.cpp`; `pr er '<command>'`)

```
peek <expr> [type] [count]   types u8 i8 u16 i16 u32 i32 u64 i64 f32 f64 ptr vec4 str wstr
poke <expr> <type> <value>   (logged at info)
vt <expr>                    vtable RVA + RTTI name
sym | single [filter]        named roots (base wcm player modules phys data chrctrl player.pos player.quat
                             player.hp player.maxhp player.dbg player.flags player.chrdbg player.team cam
                             perscam1 cam.pos cam.fwd cam.fov chrcam compasscam padman menuman menu.blocking remo
                             actionbtn dmgmod havok physworld rendman ezdraw task flipper fade loading
                             gamedataman gameman fieldarea time locktgt)
                             | every FD4 singleton (also usable as s.<Name>, e.g. [s.CSFade])
chr [n]                      characters by distance: handle, P/E/C, team, npc, hp, distance, pos
status | cam | calib | buddy | ui | ez
call setpos x y z | setyaw rad | ray x y z dx dy dz | kill yes
flag [name [on|off]]         render dead nodead nodamage nohit noattack nomove nogravity
puppet auto|off   menu auto|open|closed   debug on|off
puppet motion kinematic|root_motion   switch [Puppet] motion live
render [status | keys | set <key> <value> | probe]
hud [status | msg [info|good|warn|error] <text> | aim on|off|auto | set <key> <value>]
draw [skeleton|buddy|tracers on|off]
findq <expr> [max]           qwords equal to expr in writable private memory (class instances by vtable)
snap reset|take <label>|diff <a> <b> [max] [va vb]|add <addr> [bytes]|clear
                             live RE (er/MemDiff.h): bytes that follow a state you switch (exe .data, every FD4 singleton
                             and the objects it points to); full list in PhantomRingHost.snapdiff.txt
snap tree root <addr|player> | take <label> [depth [bytes]] | diff <a> <b> [max] [small]
                             the same over an object tree keyed by pointer path (survives reallocation across loads)
input key|keydown|keyup <vk> | mouse <dx> <dy>   SendInput inside Elden Ring's Wine desktop (keydown/keyup: Elden Ring
                             reads the keyboard once a frame)
input virtualpad on|off      test automation with no controller plugged in: user 0 reports a connected neutral pad to
                             Elden Ring's XInputGetCapabilities and polls, and Elden Ring is told devices changed
                             (WM_DEVICECHANGE): it enumerates and polls it, so `press` reaches Elden Ring itself (title,
                             menus, map, graces, prompts). Without it Elden Ring polls nothing and the bridge polls for
                             MGSV alone while a `press` lasts (`InputBridge::PollIfIdle`)
press <buttons|none> [ms] [lx ly [rx ry]]   the injected pad (lt/rt included), through the same masks as a real one
puppet hide render_flag|fade|none   switch [Puppet] hide_mode live
buddy                        the buddy, plus D-Walker's orders seen and the LB hints shown (er/BuddyOrders)
throw [status | arc on|off|auto|ini]   Snake's grenades we fly, the aiming arc
mines [detonate <id>]        Snake's placed explosives
combat shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> x y z [share]   a test shot of Snake's at an
                             Elden Ring point, as a kGEvFire from his muzzle would be (share: MGSV's v8 damage share)
combat strike x y z | aimat x y z | self <id> [off]   a CQC strike there; the MGSV yaw/pitch/camera phase that aims at
                             it (for fake_mgsv `!cam`); the Tarnished's own SpEffect <id> on/off
boom [status | lethal|stun|sleep|smoke|flame|breaker|shrapnel [radius] | mode ... | restore | pool]   looks 5 m ahead;
                             `pool`: Elden Ring's bullet/emitter counts and ours alive
boom look <kind> <bullet> [sfx] | set <key> <value> | raw <bullet> x y z   change a look live, tune the breakers, fly
                             an unmodified Elden Ring bullet from the Tarnished's chest (what does Elden Ring break?)
stealth [hide on|off | speffect <id>]   the cardboard box (er/Stealth.h)
geom [learn [use] | offset <hex> | near|all [radius [filter|-filter]] | dump <ins>]   destructible assets (er/GeomProbe.h)
cam set sync_compass 0|1     the compass write at 122
cam set collision_radius|collision_ease_out|collision_margin|vertical_smoothing <v>   camera collision / height easing
pacing [set smooth|draws on|off | set delay auto|<steps> | set extrapolate <steps> | set motion <frames>]   frame pacing
pacing probe start [frames] | stop | dump [file]   per-frame pacing measurements -> CSV next to the DLL
hud shot                     the next frame as build/PhantomRingHost.shot.bmp (half size; works with the window hidden)
```

Expressions: `+ - * ( )` and `[x]`, which reads the u64 at x, for example
`peek [[wcm]+0x1E508]+0x190 ptr`. All memory access goes through Read/WriteProcessMemory on the own
process, so a bad address returns an error and never an access violation.

### Rendering Snake (phase 2: `er/Render*`, `er/Gbuffer*`, `er/D3D12*`, `er/Shaders*`)

The guest's meshes, textures and per-frame skinning matrices (DRAW CONTRACT in
`protocol/phantomring_protocol.h`) are drawn inside Elden Ring's D3D12 frame. Nothing in `eldenring.exe` is
patched: every hook is a MinHook detour in vkd3d-proton (`d3d12core.dll`) or DXVK's `dxgi.dll`.

`[Render] stage` picks where:

| Stage | Where | Lit by |
|---|---|---|
| **C** `gbuffer` (default) | Elden Ring's own G-buffer, right after its last G-buffer list; its velocity target after its velocity pass; its shadow atlas | **Elden Ring**: sun + shadow mask, per-area light sets, sky/GI ambient, local lights (graces, torches), SSAO, fog, tone mapping, bloom, exposure, TAA. Snake casts the sun's shadow. |
| B `post` | Elden Ring's tone-mapped RGBA8 scene colour, right before the composite (the HUD stays on top) | ours: one clock-driven directional light + hemisphere ambient (`light_*`, `ambient_*`, `day_night`) |
| A `present` | the back buffer at Present (over everything) | ours, as B |

Stage C falls back to stage B for any frame whose structure doesn't match what it was built for (no G-buffer
bind with exactly these formats, unknown states, a different depth format...): `render: stage C not possible
(<why>)` once per change of reason, `stage C fallbacks` in `pr er render`.

| File | What it does |
|---|---|
| `D3D12Hooks` | Creates a throwaway DXGI factory, D3D12 device, direct queue and swap chain on a hidden window, reads their vtables, then detours `Present`/`Present1`, `ResizeBuffers(1)`, `SetColorSpace1`, `CreateSwapChain(ForHwnd)` and `ExecuteCommandLists`. It finds the presenting queue from swap-chain creation, `ResizeBuffers1`, `GetDevice`, or a direct queue whose pointer sits in the swap-chain object. Hands its dummy device to `RenderPso`. |
| `RenderPso` | Pipeline watch from DLL init: `CreateGraphicsPipelineState`/`CreateComputePipelineState`/`CreatePipelineState` and `ID3D12PipelineLibrary::Load*` (Elden Ring loads ~9300 pipelines from a library). Per PSO: vkd3d-proton's shader hashes (FNV-1 over the bytecode = the `VKD3D_SHADER_DUMP_PATH` file names), target formats, depth/stencil/bias state. `render psos`. |
| `RenderAssets` | A worker thread consumes the render ring (`kRenMesh`/`kRenTexture`/`kRenForget`/`kRenClearAll`) and validates each message. It creates DEFAULT resources and UPLOAD staging copies; our command list records the copies within a per-frame budget. Keys are the guest's content hashes; frames keep what they used until their fence passes. Each mesh's winding is measured (`RenderMesh`). |
| `RenderMesh` | Triangle winding against the vertex normals. **MGSV's meshes are wound inward** (cross(b-a, c-a) against the normal on 64 of 67 recorded meshes, the rest unknown), the fake guest's outward: the front-face setting is flipped per mesh. |
| `RenderScene` | A task at `Draw_Pre` (144) snapshots, per ER frame: the camera (`pers_cam_1`), the anchor matrix, ER's clock, riding, a private copy of the guest's `DrawList` (blended between MGSV's two lists around the moment the frame shows: Frame pacing), and the previous frame's camera/anchor/matrices per draw (velocity). |
| `RenderDepth` | The frame watch: `CreateDepthStencilView`, list `Reset`, `ResourceBarrier`, `OMSetRenderTargets`, `Clear{DepthStencil,RenderTarget}View`. Finds the scene depth (+ its stencil plane), the post-tonemap scene colour, the composite list, **the G-buffer** (5 MRTs + scene depth), **the velocity target** (R16G16_FLOAT + scene depth) and **the shadow atlases** (R16 depth, no colour); tracks their states per list, applied in `ExecuteCommandLists` order; tags lists (composite / G-buffer / velocity). |
| `RenderBind` | Binding watch: `SetDescriptorHeaps`, `RSSetViewports`, `SetGraphicsRootDescriptorTable`. Per list: Elden Ring's CBV/SRV/UAV heap and, in G-buffer and shadow passes, the per-view constant buffer descriptor (root parameter 1's table + 4) with its viewport. |
| `RenderView` | Copies Elden Ring's per-view constants on the GPU (compute shader, Elden Ring's heap bound, root UAV) into our buffer: the main view and each shadow view. Validated by readback (origin == `pers_cam_1` position, every 30th copy); until then and after a mismatch stage C uses its own camera matrices and no shadows. |
| `Gbuffer` | Stage C: the G-buffer and velocity draws, `render gbuf peek` readback. |
| `RenderShadow` | Stage C shadows: Snake and his buddy depth-only into every shadow view, with that view's constants and Elden Ring's caster depth state (`z GREATER_EQUAL, bias -2, slope -3` from its PSOs). |
| `RenderShared` | What the recorders share: frame constants (`FrameCB`), per-draw binding (textures, bones, previous bones, root constants; cached per submission), near-camera fades, pipeline base, barriers. |
| `Shaders` | HLSL compiled at runtime with Proton's `d3dcompiler_47` (vkd3d-shader): one vertex shader per input layout (decoded inputs, skinning with this and the previous frame's matrices, our camera or Elden Ring's view constants), pixel shaders `PSMain` (A/B, lit here), `PSGbuffer`, `PSVelocity`, `PSShadow` (alpha test). |
| `RenderProbe` | `render probe [shaders]`: one frame's batches, lists, passes (targets, draws, clears), with `shaders`: each pass's pipelines by shader hash, stencil refs, viewports, root signature and descriptor tables. |
| `Renderer` | Root signature, pipelines of stages A/B, upload ring, descriptor heaps, fences, the per-batch splitting, the `render` console command. |

#### Per frame

`ExecuteCommandLists` (Elden Ring's direct queue) is split around up to three submissions of ours:

1. **After the last G-buffer list** (stage C; 37-list batch #5: shadows in lists 3-10, G-buffer clears in 11,
   G-buffer draws 12-29, lighting from 30 on): the view constant copies, Snake into every shadow view, then
   Snake into the G-buffer (5 MRTs + scene depth with stencil ref `gbuffer_stencil`), states restored after.
2. **After the velocity list** (stage C; forward batch #8, list 11: a full-screen camera-velocity draw, then
   ER's moving objects): Snake's motion vectors, depth-tested (read-only) against his own depth.
3. **Before the composite list** (stage B, or stage C's fallback): Snake into the RGBA8 scene colour.

Otherwise (stage A, or nothing found) the draw happens at `Present`. Uploads of new assets go into the first
of our submissions each frame. Nothing is recorded when there is nothing to draw (no guest, no puppet, loading).

#### Stage C: Elden Ring's G-buffer (decompiled; `VKD3D_SHADER_DUMP_PATH` dumps + `render probe shaders`)

Learned from Elden Ring 1.17.1's G-buffer pixel shaders (e.g. `16e420a562886a0b`, a skinned armour material,
`f9ceb4dfb05c4045` foliage) and the lighting passes that read them (`6f88fcf1abe6b7d8` light sets,
`2f89244e8be218a4` ambient specular, `c2fdbc052a0ea337` clustered lights). Every opaque material writes the
same layout:

| Target | Format | Contents |
|---|---|---|
| RT0 | R10G10B10A2_UNORM | world normal × 0.5 + 0.5 (+0.000489, half a step); a = 0 (fade value; blending off) |
| RT1 | R8G8B8A8_UNORM | x metalness, y **gloss** (lighting: perceptual roughness = 1 − √gloss), z translucency/wrap (×10; foliage only), w vertex colour red (characters: 1.0) |
| RT2 | R8G8B8A8_UNORM_SRGB | albedo (linear into the sRGB view); a = **light-set index**/255 (lighting: `lightSets[int(a·255)·14 ..]`: 4 directional lights, colours, specular, flags, AO response) |
| RT3 | R8G8_UNORM | 0 for every opaque material seen |
| RT4 | R16G16B16A16_FLOAT | 0 for every opaque material seen |
| depth | R32G8X24 (D32_FLOAT_S8X24) | reversed Z; **stencil** = object class, written REPLACE 0xFF (refs 0x10/0x12/0x50/0x58/0x90/0xD2; lighting passes test them) |
| velocity | R16G16_FLOAT (separate pass) | (NDC now − NDC last frame) × (0.5, −0.5) (+0) |

The Tarnished (`render gbuf peek` on him, live): stencil **0x50**, light set **3** (terrain: 0), RT1.w 1.0,
steel armour metal 0.87-0.89 / gloss 0.54-0.61 / albedo 0.39-0.44, cloth metal 0 / gloss 0.004-0.125. Snake
gets `gbuffer_stencil = 0x50` and `gbuffer_ao = 1`. The light set comes from Elden Ring's per-instance data,
so `gbuffer_light_set = auto` learns it from the Tarnished himself: while he is visible (no puppet: during
MGSV's boot, after it goes) a 6×6 peek at his chest every 120 frames, the majority light set of his
stencil-0x50 texels (`render: stage C: the Tarnished's G-buffer texels carry light set N`); 3 until then. Only after
he has been on screen 90 frames, and a change needs two peeks in a row that agree: the single frame between a Stake
of Marika respawn and the puppet's restart showed him in his respawn state (light set 7: no light) and Snake and
D-Walker went pitch black for the rest of the session (VERIFIED live; fixed).

**Default now `gbuffer_light_set = 0`, Elden Ring's world set** (every terrain, rock and foliage texel carries 0). Live
(Gatefront, both games, the Tarnished hidden and no enemy in view): Snake and D-Walker were pitch black under the learned
set 3, with sets 3-6 black and 0-2 lit (`render set gbuffer_light_set N`); the very same frame with the Tarnished shown
(`render set show_tarnished on`, `flag render on`) lit them with set 3. Elden Ring lights a character set only while one
of its characters using it is on screen. Set 0 is lit whenever its world is: Snake looks a little flatter (no character
rim light), never black. `auto` keeps the learning above.

MGSV → Elden Ring material: albedo = the diffuse (BC1 holding sRGB values, decoded); normal = DXT5nm (BC3,
x in alpha); Fox SRM = R specular, G roughness, B metalness (recorded textures: fabric G 0.75-0.9 B 0, a
polished part G 0.2 B 0.8) → gloss = (1 − G)², metal = B. Diffuses that carry alpha (BC3: MGSV's hair cards,
lashes, decals) are alpha-tested at 0.5 (the guest flags none). D-Walker's armour material (`camo_materials`
= `5216a6bb`) is painted with its camo pattern (texture 3) where its mask (texture 4) is set. MGSV's
translucent draws (none in the recordings) are left out of the G-buffer.

Elden Ring's per-view constants (all its mesh vertex shaders: `BindlessCBV[registers[1] + 4]`, i.e. root
parameter 1's table + 4; vkd3d-proton gives each root parameter a push-constant slot): [3] view origin,
[24..27] camera-relative view-projection rows, [28] previous origin, [29..32] previous rows. Live: origin ==
`pers_cam_1` to 0.0000 m on every check; rows like `[24] 1.75 0 0 0, [25] 0 3.111 0.0297 0, [27] 0 -0.0095 1 0`
(fov 0.622, 16:9). Snake therefore uses exactly Elden Ring's matrices (jitter and all) for the G-buffer,
velocity and shadows; the shadow atlases get one view per viewport (4096×2048 atlas: cascades at
(0,0) 2048², (2048,0) 1024², (3072,0) 1024², plus three other R16 atlases; 4-6 views per frame). Snake is drawn
only into the sun-cascade atlas (twice as wide as high, a full-height square view at the origin:
`RenderShadow::SunCascadeAtlas`) or an atlas Elden Ring cleared in full this frame: the others (1-3 views a frame,
local lights or environment captures) may be rendered once and reused, with Snake frozen in them (the skip is logged
once per atlas; UNVERIFIED live). An atlas not bound for 60 frames is let go, so a recreated one (shadow quality) or
another area's is picked up. A watched target that changes (Elden Ring recreating its targets outside
ResizeBuffers) is kept alive until a submission of ours after the change has completed (`er/Retire.h`): our lists
hold no reference of their own (UNVERIFIED live: needs a graphics-quality change while Snake is drawn).

**Near-camera fades** (all stages, screen-door with a static 4×4 Bayer pattern that Elden Ring's TAA turns into
transparency): Snake per pixel (`near_fade = 0.25, 0.6` m from the camera), everything else per pixel
(`near_fade_other = 0.5, 1.5`) and as a whole by its nearest part, and anything but Snake with a part within
`occluder_radius` (1 m) of the camera→Snake line in front of him at `occluder_fade` (0.15; never while riding).
In MGSV's first person / scope (GuestState `kGuestFirstPerson`, `RenderSnapshot::firstPerson`) Snake's own draws are not
faded at all: they are his arms and the gun's sights right in front of the camera (session 4's "doesn't register hits
when looking down scope" was aiming blind: the sights had faded out and MGSV shows no reticle there). VERIFIED live
2026-10-12 evening: the iron sights drawn in Elden Ring's frame, two scoped darts on a soldier.
"A part" is each matrix's translation and the mesh's bind-space bounds centre carried by that matrix
(`RenderMesh::Bounds`, measured once per mesh): the translations alone are D-Walker's feet and part origins, ~1 m
under the camera line, and a walker right between the camera and Snake was never caught (VERIFIED live, both games:
now a ghost, Snake clear; `render set occluder_fade 1` shows the old solid walker).

**The buddy on Elden Ring's ground** (`RenderScene` `GroundBuddy`): its draws are moved by `Buddy::GroundOffset()`
(a ray under the mapped buddy position), smoothed, ±1.5 m, not while ridden: it walks on MGSV's flat apron.

**Console.** `render gbuf` (stage C state, view constants, shadows, binding watch), `render gbuf peek [x y [n]]`
(an n×n box of every G-buffer target, depth, stencil, light set and velocity at that pixel, decoded; default
the screen centre), `render psos`, `render probe shaders`, `render set stage|gbuffer_*|velocity*|shadows|
view_constants|near_fade*|occluder_*|alpha_*|metal_scale|gloss_*|albedo_scale|debug_view ...`.
`render set debug_offset x,y,z` draws Snake that far from the puppet and `render set show_tarnished on` +
`flag render on` shows the Tarnished: side-by-side comparisons.

#### Verified live (2026-10-12, ER 1.17.1 4K SDR, `pr replay` of the real MGSV guest: idle, run, idle_walker_behind)

- **Structure:** `render: Elden Ring G-buffer = 5 targets 0=R10G10B10A2_UNORM 1=R8G8B8A8_UNORM
  2=R8G8B8A8_UNORM_SRGB 3=R8G8_UNORM 4=R16G16B16A16_FLOAT 3840x2160`, velocity target R16G16_FLOAT, 4 shadow
  atlases; stage C every frame (`0 stage C fallbacks`), view constants trusted (48/48 checks), shadows 4 views
  per frame.
- **Snake's G-buffer texels** (peek): stencil 0x50, light set 3, normal facing the camera, albedo (0.042,
  0.042, 0.027) on the suit, gloss 0.345; velocity ≈ 0 standing.
- **Look:** Snake lit like the Tarnished next to him in a dark forest (17:43), at noon (dappled tree shadows on
  his back and launcher), at night (23:00, moonlight), on a sunny meadow (10:00), in fog (village), standing on a
  grace at night (its warm light on his legs). Screen luminance (linear, mean): forest evening Snake 0.015 /
  bush 0.018 / Tarnished 0.10 (steel); noon 0.016-0.068 / 0.039 / 0.15; night 0.014 / 0.014 / 0.11; meadow
  0.075 / grass 0.11. Before stage C (stage B, same meadow): Snake 0.062 vs 0.043 in stage C, warm-tinted.
- **Shadow:** on the meadow at 09:00 Snake's shadow darkens the grass beside him from 0.090 to 0.049 (median
  linear luminance; lit grass elsewhere unchanged 0.075 → 0.072) — `render set shadows off` removes it.
- **TAA:** running (run.prrec), Snake's velocity texels read (−0.0015, 0.0002) / (0.0024, −0.0005) with ours
  vs (−0.0002, 0.0026) / (−0.0023, 0.0095) with only Elden Ring's camera velocity (`velocity off`): ours are
  his small motion relative to the following camera; without them his outline smears visibly.
- **Fades:** with D-Walker spawned between camera and Snake (idle_walker_behind) it fills the screen without
  fades; with them it is a faint dithered ghost and Snake is clear.
- **Head:** before the winding fix the head showed its inside (and every normal faced away: Snake looked
  black); fixed.
- **Cost:** 0.09-0.12 ms CPU per frame (both submissions, shadows included), 60 fps.

#### Not done yet / next

- **Light set while the puppet runs.** It is learned only while the Tarnished is visible. Findings (2026-10-12, Gatefront
  and Agheel Lake North, `render gbuf peek` on the visible Tarnished, clock moved with `poke time+0x28`): the index is
  **not an area constant** -- on one spot it was 3 at 09:00/12:00/15:00 and 4 at 18:00-23:00 (not strictly: a second
  run gave 4 at 09:00), i.e. it follows Elden Ring's GParam transitions (a slot that flips), and both areas gave the same
  values at the same time. It is **not stored as a plain value** where it was looked for: `snap` diffs between 3- and
  4-states (4 vs 4 snapshots) found no field holding 3/4 (int, byte, 3.0f/4.0f, 3/255) in the Tarnished's object tree
  (depth 3, ~2500 objects), in every FD4 singleton and the objects they point to (~26k regions), or in
  `GXSR::GXLightManager`'s tree. Next: find where the per-instance buffer's `[+5].x` is written (the G-buffer draw's
  instance upload), or learn from Elden Ring's other characters (stencil 0x50) on screen.
- **MGSV material colours.** The sneaking suit's and gear's tints come from MGSV material constants the guest
  only partly sends (`params` = `m_materials[0..1]`); the hair needs the guest's alpha-test flag; MGSV's
  translucent draws are dropped in stage C. Camo is a per-material host rule.
- **Stage B+ (`hdr`)** is not built (stage C made it unnecessary); `stage = hdr` falls back to `gbuffer`.
- **Shadows during the death dissolve** are dropped once it passes 30% (no screen door in the shadow pass).
- **Resize and alt-tab.** `ResizeBuffers` drops all references and waits for our GPU work. It has only
  been exercised by ER's own resize after the title screen, not by a live window resize.

**Frame structure** (`render probe`, ER 1.17.1, 4K): 27-28 `ExecuteCommandLists` batches on one direct queue;
no `ExecuteIndirect`.
1. Clear of the HUD target.
2. Batch #5 (37 lists): environment captures, shadow atlases (lists 3-10, depth-only + alpha-tested casters),
   G-buffer clears (11), G-buffer draws (12-29; depth/stencil pre-passes with dithered distance fades, then the
   materials), a full-screen fix-up of RT1/RT2 (30), depth pyramids, SSAO/half-res passes and the shadow mask
   (31), lighting into two R11G11B10_FLOAT targets (32-36: light volumes with stencil marking, light sets,
   clustered lights, ambient specular).
3. Batch #8: forward/transparent passes into the R11G11B10 targets, the velocity pass (list 11).
4. Bloom pyramid (1920 down to 15×9 and back), tone mapping.
5. Post-tonemap world pass: about 20 draws into the RGBA8 scene colour with the scene depth.
6. HUD into the cleared RGBA8 target, with 736×128 text atlases.
7. One full-screen draw into the back buffer.

**Stages A/B, verified earlier** (2026-10-12, `fake_mgsv --model humanoid`): 8 detours in 134 ms; queue found
at swap chain +0x8; back buffers 3×3840×2160 `R8G8B8A8_UNORM`; the skinned model lands on the CSEzDraw
skeleton and stays there in a 2.5 rad/s orbit with `latency_frames = 0`; scene depth `R32G8X24_TYPELESS`,
cleared to 0 (reversed Z); scene colour RGBA8, the HUD on top; guest loss stops drawing the same frame.

**Live controls.** `pr er "render set <key> <value>"` (`pr er "render keys"` lists them), `pr er render` shows
the status.

**Test data.** `pr replay <recording>` plays the real MGSV guest (assets, draw lists, HUD) — use it for looks.
`pr fake-mgsv --model humanoid` sends a skinned walking figure (BC1 albedo, normal and SRM maps), a rigid
D-Walker and a fake HUD (`--hud mgsv|bar|none`, `!aim`, `!hud`, `--boot N`, `!fail`, `!ready`, `--stand`,
`--freeze T`, `--orbit SPEED --orbit-phase A`, `--cam DIST HEIGHT LOOK_Y`).

#### Verified live with both games (2026-10-12, Limgrave)

Real MGSV on the default mapping, `pr launch` + `pr er-continue`: stage C every frame (0 fallbacks, view constants
1144/1144 matched), 0.07-0.15 ms CPU/frame, 60 fps; Snake and D-Walker lit like the scene by morning light (Gatefront),
in the Agheel forest, at night under the gate's torches and the grace's light, after fast travel and Stake of Marika
respawns, and indoors (Stranded Graveyard: the Tarnished's texels there carry light set 3 too; Snake as dark as his
black armour, backlit by the light shaft, the grace's light on his legs).

### Overlay: MGSV's HUD, fallback reticle, corner text (phase 5: `er/Hud*`, `er/Overlay*`)

What PhantomRing itself shows over Elden Ring's finished frame. `er/Hud.cpp` decides it once per frame on
the task thread (FrameBegin, after the puppet's decision): a pure `Hud::Brain` turns the frame (puppet,
guest liveness and status, ER menu/cutscene/load/death, `kGuestAiming`) into opacities and text lines, so
`tests/test_host_overlay.inc` checks every rule offline. `er/Overlay.cpp` draws that model at Present, on
Elden Ring's queue, right after the renderer's own work: its own allocators, list, fence, root signature
and one pipeline per back-buffer format; every element is a textured quad with premultiplied alpha. Nothing
is recorded while nothing is visible. It needs `[Render] enabled` (same hooks); any failure turns the
overlay off for the session and leaves the frame alone.

| Element | When | How |
|---|---|---|
| **MGSV HUD layer** (`[Overlay] hud`) | Snake plays (puppet on), guest alive, no ER menu, cutscene, load or death. Fades in/out over `hud_fade_ms` (350): a guest that disappears leaves its last frame fading, not frozen. | `er/HudFrames.cpp` reads the guest's HudCtl triple buffer (reader index restarted on every attach, like the draw slots) on the Present thread, even while hidden; a new frame is copied to a per-frame UPLOAD buffer and into a DEFAULT texture. Scaled to the back buffer, aspect kept, centred, linear filter. Flags: bit0 premultiplied (else multiplied in the shader); bit1 = an sRGB render target's bytes (MGSV blended in linear light: decoded, un-premultiplied and re-encoded so it composites like Elden Ring's own HUD; `mgsv/Hud.h`). |
| **Fallback reticle** (`reticle` = auto, on or off; `reticle_scale`) | Same screen as above, while `kGuestAiming`; `auto` only while no guest HUD frame arrived in the last 1.5 s. | Four ticks and a dot, white with a dark rim, 4.5% of the screen height, drawn on the CPU (`er/OverlayArt.cpp`), fades in 0.12 s. |
| **Status line** (`status`) | Snake not playing: MGSV booting/at the title/loading → `Header.guestStatusText` (amber); failed → its text (red, kept 30 s after MGSV exits unless it recovered); ready but ER not in the world → "MGSV ready -- Snake takes over once your character is in the world" (green); no guest → "MGSV is not running -- Elden Ring plays as usual" for `absent_seconds` (12). Hidden during ER cutscenes. | Text panels: GDI draws the text (gdi32 through GetProcAddress, antialiased "Arial" = the prefix's Liberation Sans), a dark translucent box with a coloured accent bar, 1.65% of the screen height (36 px at 4K, `text_scale`), wrapped at 42% of the width, stacked from `corner` (top_right: ER leaves it empty). Made only when a line changes; cached by text/tone/size. |
| **Messages** (`messages`, `message_seconds` 4) | Puppet on → "Snake is in control"; puppet off because the guest went → "MGSV stopped -- Elden Ring has the controller". `pr er "hud msg [tone] <text>"` adds one. | Same panels, below the status line, fade in 0.25 s / out 0.6 s, at most 3. |

**Layout next to Elden Ring's HUD** (`er/HudLayout.cpp`, `[Overlay] hud_scale/hud_offset/hud_weapon_*/hud_notify_*`):
the layer is cut along two regions that move on their own, the rest stays where MGSV drew it (its reticle, prompts and
world markers must not move). The weapon box is two regions moving together (`hud_weapon_region` 0.78-1 x 0.70-1 and
`hud_weapon_bar_region` 0.62-1 x 0.84-1, its bar row): one rectangle from x 0.62 cut MGSV's call-menu help line (centred
at y 0.78, out to x 0.69) in two and moved its end up (live: "...mark nearb" | "y enemies."); now it stays whole (live
screenshot). While MGSV's own menu is up (kGuestInMenu: the iDroid, its pause menu) the layer is one full-screen picture
and is drawn uncut. Measured on the real recordings (1280x720 layer) and Elden Ring's HUD (fractions of the
screen, so 4K and 1080p are the same): MGSV's weapon box x 0.66-0.92, y 0.78-0.91; Elden Ring's rune counter y 0.94-0.98
(and the "+N runes" of a kill above it); MGSV's notifications ("Development Complete [TACTICAL (FULTON)]") x 0.09-0.34,
y 0.58-0.61; Elden Ring's skill label y 0.63-0.65 and equipment slots from 0.74, both x < 0.17. Default: the weapon box
(region 0.62-1 × 0.70-1) moves up 5% (bottom at 0.86); the notifications stay (0.02 above the skill label; -0.03 lifts
them). VERIFIED live (`hud shot`, run.prrec, Elden Ring's HUD forced on with CSFeMan+0x78 = 1): weapon box at y
0.73-0.86 with the rune counter clear below, the notification at y 0.59 above where the skill label goes; 8 pieces per
frame. `hud set hud_weapon_offset 0, -0.08` etc. live.

Console: `hud` (model + draw counters), `hud set <key> <value>` (the `[Overlay]` keys), `hud aim on|off|auto`
(force `kGuestAiming` for a test), `hud msg`. Cost: inside the renderer's 0.05 ms/frame CPU average; one
1280x720 copy (3.7 MB) per new guest HUD frame while the layer shows.

**Verified live (2026-10-12, ER 1.17.1 4K SDR, `fake_mgsv --model humanoid` on `/dev/shm/phantomring_h`).**
- Title screen and in the world while the fake boots (`--boot 70`): "MGSV: loading Africa free roam (30020)"
  top right, crisp at 4K; the Tarnished plays normally meanwhile.
- Ready: `puppet ON`, "Snake is in control" (green), log `overlay: first MGSV HUD layer (1280x720 -> 3840x2160)
  drawn`; the fake's ammo box sits bottom right over Elden Ring's frame, ER's compass stays.
- `!aim on`: the fake HUD's own reticle at the centre; `!hud none` + aiming: the fallback reticle instead.
- Start (ER menu): the HUD layer is gone, ER's menu is clean; closing the menu brings it back.
- `!fail <text>`: the red failure line; guest killed: "MGSV stopped -- Elden Ring has the controller", the
  Tarnished visible and playable, the HUD layer faded out.
- First build: GDI under Proton inks the last glyph past its measured width ("Snake is in contro|"); panels
  now wrap with the measured box, draw with DT_NOCLIP into slack and crop to the ink (fixed, re-checked).

### Death visuals (`er/Puppet.cpp`, `er/RenderScene.cpp`, `[Render] death_visual`)

**Dying** (`Puppet::Dying`, the status line's `dying`): from the death until Elden Ring has respawned the player.
Not the dead bit: Elden Ring clears it ~6 s after YOU DIED, while the screen is still black and long before its
respawn load -- or before "Choose revival location" (near a Stake of Marika), which waits for the player (VERIFIED
live). The puppet restarted on the cleared bit and stood Snake up in the middle of his death. A respawn is now a
living player after a load (or a different player object), or 15 s alive without one, never while an ER screen is
up; the puppet doesn't start under a blocking screen; HostState `kHostPlayerDead` and MGSV's neutral pad last the
whole time. The 15 s only count while ER's HUD signal is known (seen visible since the last load): with ER's HUD
switched off the revival dialog is invisible to us, so a respawn without a load then needs the player moved 5 m from
where it died (a stake revival), or 120 s alive (`er/Revival.h`; UNVERIFIED live with the HUD off).

The same rule holds when **the Tarnished dies while Snake isn't playing** (MGSV booting or restarting, the player
playing the Tarnished meanwhile): `Decide` enters the dying state on the dead bit of an inactive puppet (`puppet: the
Tarnished died while Snake wasn't playing -- Snake takes over after his respawn`). Before, nothing was dying, the
cleared dead bit looked like a living player and the puppet started under "Choose revival location" (live 2026-10-12
evening: Snake's HUD over the dialog). Fixed after that was seen; not re-seen live.

From the moment the puppet is killed for Snake's death (`ChrIns::Kill`), or Elden Ring kills the puppet
itself, until the respawn or the character changes:

- Snake (and his draw list: gear, D-Walker) keeps being drawn at the puppet's last anchor. **`keep`** (default
  since the guest's `[Combat] DeathCollapse`): as MGSV shows him -- he dives and lies prone until the respawn,
  stands up after it (VERIFIED live: prone in front of YOU DIED). `fade` dissolves him over `death_fade_ms`
  (900) with a screen-door dither (interleaved gradient noise, `FrameCB.gOutput.w`) that keeps depth and
  self-occlusion right; `cut` drops him at once.
- **The Tarnished stays hidden** (the puppet stops with the render bit still cleared, re-asserted each frame;
  `Start()` keeps the original saved values when it re-hides the same character): he no longer dies on
  screen in Snake's place. Restored at the respawn when Snake does not take over again.
- The MGSV HUD layer and reticle hide (death counts as "not Snake's screen").

Verified live: `!die` → `killed the ER player`, `puppet OFF (player dead)`; a screenshot ~0.5 s later shows
Snake and D-Walker half dissolved, ~2 s later nothing but Elden Ring's world (no Tarnished), 6 s later `ER
player respawned`, "Snake is in control" and the HUD back.

For the MGSV side (another agent's): a fall/faint animation needs the guest to play MGSV's own down/dying
motion while `kHostPlayerDead` is set (Snake is kept at life 1 there today) and to stop it on
`kHEvRespawn`; then set `[Render] death_visual = keep`.

### Player defaults

`[Draw] skeleton/buddy/tracers` default to false (developer views; `pr er "draw skeleton on"` or the ini turn
them on), `[Render] xray = 0` and `debug_view = 0` as before. `[Axes] mirror_x` now defaults to -1 in the
code as well as in the ini (the shipped ini must equal the built-in defaults; the test caught the mismatch).
Screenshots of a normal session show no CSEzDraw lines, no x-ray, only Snake, D-Walker and the overlay.

### Lighting from Elden Ring (done by stage C; earlier findings)

Stage C lets Elden Ring's own deferred lighting light Snake (above), so the stages A/B light (`day_night`,
`light_*`, `ambient_*`) is only the fallback's. What the lighting passes do with the G-buffer (decompiled):
the light-set pass (`6f88fcf1abe6b7d8`) indexes a table of 14 float4 per set by RT2.a — 4 directional lights
(direction + a size term, colour, specular colour, enable flags, AO response); light 0 is scaled by the sun
shadow mask (`.w` of the AO/shadow texture) — then GGX specular with the gloss above; clustered point lights
(`c2fdbc052a0ea337`) read a light list buffer; ambient specular (`2f89244e8be218a4`) combines an env-BRDF LUT
with SSR. Earlier memory findings (`GXSR::GXLightSet`, vtable RVA 0x2F16AE0: four directional lights from
+0x40, 0x60 apart; two instances that did not change with the clock 06:46 -> 13:00, so fixed GParam sets;
`GXSR::GXSkySimulation` 0x2F29478, `GXSR::GXLightManager` 0x2F116C8) stay valid but are no longer needed.
The ER clock can be moved live for tests: write `WorldAreaTime` +0x2C minute and +0x30 second, then +0x28 hour
(all three; `pr er "poke time+0x2C u32 0"`, `"poke time+0x30 u32 0"`, `"poke time+0x28 u32 23"`).

### Keyboard and mouse

These are left to Elden Ring for now. Phase 5 forwards them to MGSV's raw-input handler.

### Offline verification

- `tests/test_host.cpp` (wine) covers:
  - the console expression parser;
  - the anchor math (round trips, delta/heading consistency for every erForward/mirror combination);
  - the kinematic controller against a synthetic world (flat walk, wall stop at radius, slide, 0.3 m step, 1 m block, ledge fall + landing at ~9.9 m/s, no drift), `Enclosed()` and the unsticking (back to where it stood free, across an origin shift);
  - the link against a scratch mapping (attach/alive/foreign nonce, GuestState, events);
  - the console with no game (peek/poke of own memory; game symbols answer "unavailable").
- `tests/test_host_overlay.inc` (included by test_host): every overlay rule (status/boot/failed/absent
  lines, messages, HUD fade on menu/death/guest loss, reticle auto/on), the reticle picture and a GDI text
  panel under wine (`PHANTOM_DUMP_OVERLAY=<dir>` writes them as raw RGBA for a look).
- `tests/test_host_combat.cpp` (wine): the damage model (bigness, reference HP, MGSV's share, headshots, a field boss's
  10-20% per magazine), explosion looks, the breaker / flame / object-only attack rows (synthetic and on the real
  regulation: the flying Fire Pot's own attack is object-only, ours become so), `Stealth::BoxHidden`, `Rounds::FlightOf`.
- `tests/test_host_load.cpp` loads the DLL into a non-Elden Ring process. It must log `exe mismatch` and do nothing else.
- `tests/test_render.cpp` (wine): every renderer shader (VS per layout, `PSMain`, `PSGbuffer`, `PSVelocity`,
  `PSShadow`) compiles with Wine's d3dcompiler and its `FrameCB`/`DrawCB` sizes (D3DReflect) equal the C++
  structs; the pipeline-stream parser and vkd3d-proton's shader hash; mesh winding detection and the
  front-face rule; near-camera, owner and occluder fades; stage C's finite reversed-Z mapping (depth 1 at the
  near plane, 0 at the far one) and the light-set default.
- `cmake --build build --target check_host` runs test_common, test_host (plus: the shipped
  `er/PhantomRingHost.ini` equals the built-in defaults), test_host_combat, test_render and test_host_load
  from `build/`.
- `wine build/verify_er_addresses.exe '<eldenring.exe>'` runs the address table against the exe on disk:
  70/70 (0 failures, 2026-10-12).
- `tools/phantomring/test_plugins.py`: the real host link + console (`test_host.exe --peer`) against
  `fake_mgsv.py`.
- `python3 tools/er_aob_scan.py verify` re-checks the catalog the address table mirrors.

### Live checklist (UNVERIFIED until the first run)

The ordered, step-by-step plan is `docs/LIVE_TEST_P01.md`; this is the host's part of it.

1. The log shows `exe identity OK`, `address table: 70/70 verified`, the singleton finder count with no name warnings, `system init done`, and 4 `task ... registered`. `pr er status` shows the task counters climbing.
2. The XInput hook works: `input: publishing XInput pad N`. `pr status` shows the pad. ER ignores sticks and face buttons while the puppet is on. Start opens the ER menu, and the menu can be navigated (latch, then a `learned ER menu UI state` line). Pin those indices.
3. Calibration lines appear after running around without the guest: `ER character forward`, `camera +0x30 row`, and a camera distance of 2–6 m.
4. The puppet starts (`puppet ON`), and the Tarnished disappears. If the hitbox or shadow also disappears, or nothing hides, try `hide_mode = fade`.
5. Motion follows Snake: walls stop it, it slides, it climbs steps under 0.5 m, it falls off ledges. `ray_filter 0x2000058` must hit the world. Check with `call ray <pos> 0 -5 0`.
6. The skeleton stands on the puppet (green, CSEzDraw), faces like Snake, and limbs are not mirrored. If not, adjust `[Axes] mirror_x`, `mgsv_yaw_sign` and `mgsv_root_offset_y`. If nothing draws, check `ez`.
7. Camera: ER renders from `pers_cam_1` written at group 107 (RE E3). If not, try `task_group = 144`. Also check the fov units (the log prints ER's own value) and the forward row sign. Walls pull the camera in.
8. Hits on the puppet produce `kHEvHurt` (debug log), and the HP bar mirrors Snake's life. Snake's death triggers YOU DIED, and the respawn sends `kHEvRespawn`.
9. Buddy: `buddy` shows D-Walker's type and flags. The cyan marker sits on the ground next to Snake. Riding raises the speed and step limits. Buddy shots are traced.
10. A guest heartbeat timeout (kill MGSV) hands control back the same frame: `puppet OFF (guest lost)`.
