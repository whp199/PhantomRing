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

1. **Exe identity.** TimeDateStamp and SizeOfImage pick one of the supported builds (`er/Addresses.h` `kBuilds`): 2.7.2.0
   (App 1.17.2: `0x6AABF7E7` / `0x5DFD600`) or 2.7.1.0 (App 1.17.1: `0x6A96B418` / `0x5E0DA00`). With `verify_sha256`, it
   also checks that build's sha256. Any other exe gets one `exe mismatch` line in the log, and the DLL stays inert.
2. **Address table** (`er/Addresses.cpp`, one per supported build; see *Elden Ring builds* below). Each entry is checked
   against this exe:
   - globals: a unique AOB that resolves to the expected RVA;
   - functions: the expected bytes at the RVA (for jmp thunks, the jump target too);
   - vtables: the RTTI name behind `vtable[-1]`.

   A failed entry is logged and stays null. Every feature checks what it needs and turns itself off rather than guess.
3. **Shared memory** (`er/Link.cpp`). It opens or creates `/dev/shm/phantomring_v1` (OPEN_ALWAYS, full size) and clears magic while it rewrites the header. It stamps the version, a fresh BCrypt-random `sessionNonce`, the QPC frequency and `hostUnixPid`: under Wine from `/proc` (`HostUnixPid`: the wineserver's pid from `Z:\proc\self\stat`, then
the first `Z:\proc\self\task\<n>` above it that exists -- Wine looks paths up in the calling process, so only Elden Ring's own
threads do -- and that thread's `Tgid`; ~100 probes; with a long-lived wineserver far below, as system Wine's in the offline
tests, from `/proc/loadavg`'s newest pid down while a fresh thread of ours waits; written to `PHANTOMRING_HOST_UNIX_PID`), never
the raw getpid syscall stub
of `common/Mapping.h UnixPid` any more: Proton experimental-11.0-20261027b handles a syscall made outside its own modules as a
Windows one, and the stub crashed Elden Ring as the host attached (an access violation at the stub + 4, live 2026-10-31; MGSV's
guest met the same, mgsv/Link.cpp). Real Windows: `GetCurrentProcessId`. It resets every host-owned region: HostState, PadState, the host event ring, ActorTable, and the console (`respSeq = reqSeq`). It drains what it consumes: guest events and the render ring.
4. **XInput bridge** (`er/InputBridge.cpp`). MinHook detours `xinput1_4.dll` ordinal 2, the import ER uses. This is a system DLL, so nothing in `eldenring.exe` is patched.
5. **System init.** It waits for `global_hinstance` and the `CSTaskImp` instance, as fromsoftware-rs does.
6. **FD4 singleton finder** (`er/Singletons.cpp`, TGA's method in-process). It names every singleton slot through the game's own `get_singleton_name`. It cross-checks the AOB-resolved slots by name and gives the console `s.<Name>` symbols.
7. **Tasks** (`er/Tasks.cpp`). A RecurringTask-shaped object is registered with `register_task` in four groups.

### Elden Ring builds

Each supported `eldenring.exe` has its own address table and `Layout` (`er/Addresses.cpp` `kTables`); the identity check
picks one. RVAs named elsewhere in these docs and in code comments are 2.7.1.0's (the build PhantomRing 0.1-0.4 was made
and live-tested on) unless they say otherwise; `tools/er_aob_scan.py verify` prints every catalog entry for whichever
build it is given.

**2.7.2.0 (App 1.17.2, Steam build 25827938, 2026-10-21).** Ported offline 2026-10-26, byte-verified, then run live the
same day with both games (me3 0.13.0 unchanged; `address table (eldenring.exe 2.7.2.0): 80/80 verified`; the regression is
in [STATUS.md](STATUS.md#elden-ring-1172-eldenringexe-2720-2026-10-26-live-regression)). The port:
- The 2.7.1.0 image was rebuilt from the Ghidra project and diffed against 2.7.2.0. Every function in the table (and the
  callees whose behaviour PhantomRing relies on) has the same reachable code instruction for instruction, rel32/RIP
  operands aside; every global is the same FD4 singleton by name (all moved by `0x3020`) or the same site; every vtable is
  the same RTTI class with the same slots (PlayerCtrl slot 12, CSPlayerDamageModule slot 7, the snipe collector's six).
- New: FD4PadManager's AOB (2.7.1.0's ran into an Arxan stub, whose bytes differ per build), cast_ray's world-cast call
  bytes (its rel32), and the RVAs. The task group table (169 groups) is identical.
- **WorldChrMan grew by 0x10** (WorldChrManImp `0x1F3E0` -> `0x1F3F0` bytes; two new pointers at `+0x1E600`): every field
  from `+0x1E600` on moved by 0x10, so `chr_cam` is `+0x1ECF0` (was `+0x1ECE0`) and the by-distance list `+0x1F1E0` (was
  `+0x1F1D0`): `Layout`. `main_player` `+0x1E508` and everything below stayed. Found by the classes' deleting-destructor
  sizes (the only one of ~4,000 that changed) and the field accesses after every load of each singleton slot the host uses
  (all identical but WorldChrMan's).
- Other code changes met on the way (none touches what the host reads): ChrIns::ApplySpEffect's worker calls a function
  (`0x3F6000`) that now special-cases SpEffects 503041 and 1673020-1673034, and ReceiveDamage's worker (`0x445650`) four
  attack ids 10673010-10673013.
- Regulation: one Bullet row changed (20003309, `atk_id_bullet` 0 -> 1); every row PhantomRing reads or patches is
  byte-identical.

### Per frame (ER task groups)

| Group | Work |
|---|---|
| FrameBegin (0) | heartbeat, guest liveness (`guestNonce == sessionNonce` and heartbeat < 3 s), guest status text → log, GuestState snapshot, frame pacing (this frame's moment on MGSV's timeline and the blended motion: Frame pacing below), menu watch, dev console, guest events (then decoys, the suppressor watch `Noise::Tick`), puppet start/stop, input mode, draw-list acquire, host status |
| ChrIns_PrePhysics (78) | world shift check (below), then puppet motion: (paced) odometer delta × Ry(delta) (+ X mirror), then either the kinematic controller (`er/Kinematic.cpp`: knee/chest wall rays with radius kept perpendicular to the wall, one slide along the estimated wall normal, step-up, ground snap, gravity) + `ChrIns::SetPosition`, or (`motion = root_motion`) the displacement handed to ER's own proxy as root motion (`er/RootMotion.cpp`); `ChrIns::SetYaw` |
| DrawParamUpdate (107, `[Camera] task_group`) | a fresh GuestState into the pacing history; anchor feet re-read after ER's physics; `CSCamera.pers_cam_1` ← MGSV's (paced) camera at the feet (height eased); eye→camera rays pull it in front of walls (out eased); Elden Ring's HP/FP/stamina bars (`ErBars::Apply`, `[Overlay] er_bars`) |
| WorldChrMan_PostPhysics (117) | hide/NoDead re-asserted (and a respawn's new Tarnished hidden from his first frame), HP mirror + `kHEvHurt`, `kHEvRespawn`, axis self-calibration (only while ER drives the player), ActorTable, buddy, debug draw, HostState |
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

Nor while the Tarnished has 0 HP (`Puppet::Decide`, logged once `puppet: the Tarnished has 0 HP (a revival screen after a
load?)`): a save Elden Ring wrote while he was dying loads him under its "Choose revival location" screen with the dead bit
clear, and Snake used to take the pad there (README Known issues); a living player never has 0 (the puppet's NoDead keeps 1).
UNVERIFIED live that such a load reads 0.

The puppet stops, and everything it changed is restored, the same frame any of these happens:

- the guest heartbeat times out;
- a load starts;
- the player dies;
- the player object changes;
- the player takes the Tarnished (Tarnished mode, below), or Elden Ring walks him through a mist (the step-aside, below);
- the console says `puppet off`.

Each reason that keeps the puppet off is its own `Puppet::Hold` (console, Tarnished mode, step-aside); it runs again when
none is left (`puppet auto` clears the console's and a step-aside).

Elden Ring has the controller back on that frame.

**Menus**: the primary signal is CSMenuMan+0x798 (non-zero while a blocking menu is open; re-backlog
E4, code-verified, coverage checked live; every change is logged as `ER blocking menu open/closed`).
`er/MenuWatch.cpp` stays as the fallback and OR'ed in: after a Start or View press (or a keyboard menu key, below), a latch
(1.5 s) treats an ER menu as open. Meanwhile CSMenuMan UI states that turn visible after
the press, and are still visible when the latch ends, are *learned* as menu states. The log says
`learned ER menu UI state 0x..`; pin those values in `[Input] menu_ui_states`. Entries learned or armed together form a
group; an entry also seen as Elden Ring's HUD (turned visible with no press and no menu open, or already up when the press
came: `HUD too` in `pr er status`) neither keeps its group open nor closes it (`MenuWatch::GroupOpen`, tested): a banner
that came up inside the latch no longer hands the pad back to MGSV inside a menu, and a boss bar no longer keeps the map's
group open. While an ER menu is open,
ER gets the whole pad and MGSV gets `kPadNeutral`. The same holds while an **ER cutscene** plays
(`[[CSRemo]+8]+0x14C` bit 0, E4; logged as `ER cutscene playing/over`, `kHostCutscene` set, puppet motion
paused). A press during a cutscene, or within 500 ms after it (the skip's fade: `MenuWatch::SwallowPress`), never latches
or learns, and whatever turns visible within 4 s after it is HUD and never arms or teaches a menu, even inside a latch
(`WindowArm`, tested): skipping Margit's intro with Start once learned his health bar as a menu and Elden Ring kept the pad
for the fight (e226ea5). A later press in those 4 s still latches (it used to be swallowed, leaving MGSV the pad inside a
menu opened right after a cutscene), and nothing is un-learned inside a latch. Re-checked 2026-10-13 without the cutscene (it plays only the first time): through the mist, Margit's bar came
up and the menu watch stayed closed (`menu closed`, the bar's entries `HUD too`), the pad Snake's for the whole fight.

**Full-screen menus** (`MenuWatch::FullScreenMenu`, tested; found live 2026-10-13 with both games): Start -> Inventory (or
Equipment, Status, the map) hides the main menu's own entries (0x1c, 0x17) and, a frame later, Elden Ring's in-game HUD (UI
state 7, `interaction_hud_state`); the inventory's entries (0x1f, 0x20; the equipment's 0x1d) come up in between. The main
menu's group closed, the new entries were taken for HUD, and A/B/D-pad went to MGSV inside the inventory (D-pad right switched
Snake's weapon; only Start got out). Now: the HUD hiding while a menu is open, or was within `kSubmenuGapMs` (500 ms), makes
it a full-screen menu, open until the HUD is back (`menu watch: Elden Ring's HUD hid inside its menu ...` / `... is back`);
entries that come up in the frame their menu's group closes arm a group of their own and are learned (`learned ER menu UI
state 0x1f (a submenu replacing its menu)`), so the menu never reads closed for a frame -- that one frame had Elden Ring's pad
lose the A still held and see it pressed again: one A opened the inventory *and* the first item's context menu, one A in
Equipment opened the right-hand list (live; with the fix: the equipment overview and the inventory grid, B back to the main
menu, B closed, MGSV's HUD layer hidden throughout, VERIFIED live). The HUD state itself is never learned as a menu entry
(a close press used to learn it as the HUD came back). Such a submenu group needs one entry that is not also Elden Ring's HUD
(`MenuWatch::SubmenuGroup`, review 2026-10-13): closing the map during Margit with his bar (0x12, HUD too) coming back in the
same frame would otherwise arm a group of the bar alone, held "open" for the whole fight (and learn 0x12 as a menu).

**Dialog boxes** (`[Input] dialog_ui_states` 0x2b, `MenuWatch::DialogOpen`, tested; 0.6.0): Elden Ring's yes/no questions --
Memory of Grace's "Lose all runes to return to the last site of grace visited?", the map's "Traveling to ... Is that okay?" --
are CSMenuMan UI state 0x2b on 1.17.2 (`03`, then `07`; 0 without one), found live 2026-10-31 from `pr er ui` with the travel
question up and gone. While it is visible Elden Ring has the whole pad, however it came up. Reproduced live first (Snake playing,
fake MGSV, Stormveil Cliffside): Start -> Inventory -> Tools -> Memory of Grace -> Use closed the inventory and put the question
up with no press and no menu open, 0x2b already "HUD too": `menu closed`, MGSV's HUD layer back over the question, the pad
Snake's (B reloaded in MGSV, nothing answered it). With the fix (Stormveil, the same path): `menu watch: Elden Ring's dialog box is
up (a question) -- Elden Ring has the pad until it closes`, MGSV's HUD layer hidden, B during it -> MGSV's pad `Connected|Neutral
buttons 0`, A on NO -> `Elden Ring's dialog box closed`, the pad Snake's again, runes and place unchanged. (B doesn't close this
question in Elden Ring itself: A on NO does.) Dialog states are never learned or un-learned as menu states.

**Keyboard menus** (`[Input] menu_keys` 0x1B Esc, 0x47 G = its map; roadmap 1.8): Elden Ring opened its menu from Esc and
nothing noticed it -- MGSV's HUD layer and the throw markers stayed over it, and its UI entries, coming up "without a
press", were even taken for HUD. Each frame `MenuWatch` reads those keys (`GetAsyncKeyState`: the down edge, or the
"pressed since the last call" bit for a tap shorter than a frame), only while Elden Ring's window is in front, and counts a
press like the pad's Start. VERIFIED live 2026-10-13 (`input keydown/keyup 0x1B`): `menu watch: key 0x1b (Esc: its menu)
pressed in Elden Ring's window` -> `learned ER menu UI state 0x1c/0x17`, `menu OPEN`, the MGSV HUD layer at 0; Esc again
closed it and Snake's HUD came back.

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
it was up at the press (or came up after it: a rest shows it at the sit-down, 0.7.0) and was seen visible since the last load; while
our `[Overlay] er_bars = none` hides the bars UI state 7 reads hidden too and is ignored ("Elden Ring's bars" below): a hidden HUD is not a menu signal by itself (ER's
"auto" HUD setting may hide it while you just play -- UNVERIFIED which state that hides). The press-less starts (an NPC's
dialog, the next screen, the grace's next screen) trust a HUD hide, so `AutoHud` (er/Interaction.h, review 2026-10-13) counts
the hides nothing explains (Snake playing in the world: no press, menu, cutscene, load or death, no prompt or interaction end
within 1 s, not at a grace, no rest prompt): at the second it logs `Elden Ring's HUD looks set to "Auto"` and turns those
starts off for the session (the Tarnished-mode gate then uses "at a grace" instead of the HUD); and a press-less start whose
Tarnished walks 2 m away ends at once (no screen keeps him moving). Live pass B 2026-10-15 (the save's HUD is "Auto"): its fade
doesn't touch UI state 7 (100 s idle in Tarnished mode, the HUD gone from the frame, the state still "playing"); but once, in
Tarnished mode next to Gatefront's soldiers, the state hid with nothing on screen after a map press Elden Ring refused, and the
chord then only woke it (Snake came back on the second try). Since then a hide counts for the Tarnished-mode gate only when one
of Elden Ring's screens explains it as it hides (`g_hudHideScreen`: a menu, an interaction, a cutscene, a load, the grace, an
action prompt within 2 s, the rest prompt's window); the log says `ER HUD hidden ... -- nothing on screen explains it`
otherwise. Live: Y at the Gatefront
grace -> its menu, B -> Snake plays on; Y at a bloodstain -> runes back; a pickup -> Snake back after 3.5 s. Until the HUD has
hidden, Elden Ring gets only the face buttons, D-pad, Start and View (`InputBridge::SetButtonsOnly`; an Elden Ring menu
opened meanwhile gets everything): with the whole pad the stick, bumpers and triggers walked, rolled and swung the hidden
Tarnished for the rest of the 3.5 s after a 1 s pickup, and Snake then jumped there (UNVERIFIED live).

**The mist** (`[Input] release_prompts` 10000 "Traverse the mist", `release_ms` 4500): Elden Ring walks the Tarnished
through a fog wall itself, and refused to while the puppet held him (the player's session). The prompt press turns the
puppet off for `release_ms` (`Puppet::Hold::kStepAside`), then Snake takes over where Elden Ring left him. VERIFIED live
2026-10-13 at Margit's fog, kinematic and root motion: `puppet steps aside for 4500 ms` -> the Tarnished walked through
(screenshot: inside the arena, Margit's bar up) -> `puppet back after Elden Ring's walk`, `puppet ON ... ER feet (-14.03,
0.96, -12.09)` inside, 2.2 m past the wall. Before the SetYaw fix (Motion below) the prompt never even showed while Snake
played: it checks the player's facing, and the puppet was pitched.

**NPC dialogs that come up by themselves**: live, resting at the Castleward Tunnel grace brought Melina, who talked and
then asked "Go to the Roundtable Hold" (A/B) with no press of ours: the interaction (3.5 s, HUD still up) was over and the
pad was MGSV's, so nothing could answer her (Tarnished mode did). Now Elden Ring's HUD hiding while (or within 1 s after)
an interaction prompt was on screen, with no press, no menu and no cutscene, starts an interaction too (`ER dialog: its
HUD hid as the prompt (ActionButtonParam 6001) went, with no press`): Elden Ring has the pad until its HUD is back. One of
its screens right after another continues it: answered, her question gives way to the grace menu with the HUD back for
~120 ms in between, and an interaction past its minimum time ended there (Snake had the pad inside the grace menu); the HUD
hiding again within 1 s of an interaction's end now continues it (`ER interaction continues: its HUD hid again 116 ms
after (the next screen)`). VERIFIED live 2026-10-13 (the session-start save restored three times to bring Melina back,
fake MGSV, Snake resting with Y): her talk, `ER dialog ... 6001`, B as Snake declined her, the grace menu followed (chained
after an 8 s wait in the dialog), B left it, Snake had the pad again; MGSV saw no press of it.

**Sitting at a grace** (`g_atGrace` in `er/Host.cpp`): from a rest until the player is 2 m from where he rested (or a load,
a death), Elden Ring's HUD hiding is always one of the grace's screens and continues the interaction, however long the HUD
was back. Live with both games 2026-10-13: "Pass time" -> "Until nightfall" showed the HUD for 4.7 s through its fade and then
the grace menu again; the 1 s chain above missed it, Snake had the pad inside the grace menu (B reloaded his rifle, nothing
could close the menu). Now `ER interaction continues: its HUD hid again 4682 ms after, Snake still at the grace (its next
screen)` -> ER has the pad -> B, B -> `ER: left the grace (2.0 m from where he rested)` (VERIFIED live, real MGSV).

**Resting** (`kHEvRested`, protocol v10; `[Input] rest_prompts` 6101 "Rest at site of grace", from the player's log: the
HUD hid ~3 s after it, the grace menu): Elden Ring refills the flasks when the player rests, so the guest refills Snake's
ammo. `a = 0` for a rest: Snake's prompt press on a rest prompt, the Tarnished's own Y on one (Tarnished mode: the bridge
still sees the real pad), or Elden Ring's HUD hiding within 6 s of the rest prompt (a keyboard rest; not when one of its
menus hid it -- live, the map hides it too); `a = 1` with every respawn, next to `kHEvRespawn`. VERIFIED live 2026-10-13:
`ER: rested at a grace (Snake's Y on its rest prompt ...) -> kHEvRested 0` -> fake MGSV `ammo 12+120 -> 30+120`; in
Tarnished mode `(its HUD hid 2.7 s after the rest prompt)`; after Margit killed Snake `ER player respawned (18.9 s after the
death, after the load) -> kHEvRespawn, kHEvRested 1`.

**View: Elden Ring's map or MGSV's iDroid** (`[Input] view_hold_ms`, 400): while Snake plays, a View *tap* still opens
Elden Ring's map, but Elden Ring only gets it once it is released (a 120 ms synthesized press), so a View *held* 400 ms
goes to MGSV instead (its iDroid) and Elden Ring never sees it (MenuWatch's Start/View latch only counts the presses
Elden Ring got). Logged as `input: View held 400 ms -> MGSV (its iDroid; N so far)`. VERIFIED live with fake_mgsv: tap ->
ER's map, MGSV no Back; 700 ms hold -> MGSV's Back, no ER menu. Re-verified 2026-10-13 with the gate below (tap: map, its
UI state 0x3d learned; 700 ms: `View held 400 ms -> MGSV Start held 1200 ms`, fake `Start pressed`, no Elden Ring menu).
**Since 0.6.0** (`[Input] idroid_open_ms`, 1000; `PadGate`, `TestQuickItem`): a View held past `view_hold_ms` opens the iDroid
when it is let go, or once held `idroid_open_ms` -- no longer at 400 ms while still held: "hold View, then press X" (the flask)
only drank within 0.4 s of View, later the iDroid had opened and X went to MGSV inside it. View held 0.4-1 s then X: the drink,
no iDroid (not at the release either), no map, no pause; View let go after 0.4-1 s: the iDroid at the release (MGSV's Start held
1.2 s); still held at 1 s: the iDroid as before; a tap: the map. View then Start within 1 s is now the chord with both still
undecided (no iDroid to cut). `idroid_open_ms = 0` brings back the old timing. Logged `input: View held 400 ms+ (let go | still
held after 1000 ms) -> MGSV Start held 1200 ms`. Live 2026-10-31 (fake MGSV `!idroid on`, the virtual pad): View 600 ms then X ->
`flask: View + X -> the hidden Tarnished presses his item button` -> `Elden Ring healed 250 HP (37% of 680) -> Snake +2204 life`,
`Flask of Crimson Tears used 924 ms after the press, 3 left`, no iDroid; View 600 ms let go -> `View held 400 ms+ (let go) -> MGSV
Start held 1200 ms`, `MGSV menu open`; View held past 1 s (the console's X came after it) -> `(still held after 1000 ms)`, the iDroid,
no drink; a 120 ms tap -> Elden Ring's map (frame); View + D-pad down 700 ms in -> `the quick item is now slot 1: Flask of Cerulean
Tears`. **In Tarnished mode** X and D-pad down are Elden Ring's own; an item / cycle press while View is still undecided now only
keeps View from Elden Ring (no map with the drink: live, View then X ~250 ms later -> the Cerulean flask drunk by Elden Ring's own
X, `menu false`, no map). **Keyboard** (`input kbm on`): Tab held 0.6 s then Space -> `flask: View + X ...`, healed 90 HP, `2 left`;
Tab + 2 -> the next quick item. The switch's corner line names an emptied flask "(empty -- rest at a grace)" (live it said
"Flask of Cerulean Tears (1)" for the drunk-out flask: its empty twin's entry counts 1; `quickitem::CycleText`, tested).

**Start, View and Tarnished mode** (`er/Chord.h` `PadGate`, pure, `tests/test_host_chord.inc`; roadmap 1.1). Everything
Snake can't do yet -- a flask, the Spirit Calling Bell, Margit's Shackle, Torrent's whistle, jumps, ladders, an NPC's
question -- the player now does as the Tarnished, without leaving the game: **View + Start held together `[Input]
chord_ms` (1000)** hands the controller to the Tarnished (`Puppet::Hold::kTarnished`: the puppet stops through its usual
path, he shows with his weapon, Snake's draws and MGSV's HUD layer go), `kHEvTarnished 1` parks the guest (neutral pad, no
follow whistles), and the corner says "Tarnished mode -- View+Start to return to Snake". The same chord gives Snake back:
the puppet starts again where the Tarnished stands (its start anchors him there; D-Walker is anchor-relative and comes
along), `kHEvTarnished 0`, "Snake is in control". A guest that attaches meanwhile hears `kHEvTarnished 1` again. What the
Tarnished went through is Snake's too: the HP bar's net change between the two toggles goes to the guest as `kHEvHeal` or
`kHEvHurt` (the bar is Snake's life again from the next frame on). Console: `tarnished [on|off|toggle]`.
- Start and View pass through one gate on every poll of the pad. **kSnake** (Snake plays, no menu): View as above; Start
  reaches Elden Ring `[Input] start_wait_ms` (150) after it went down, or at its release if that comes first (a tap: a
  synthesized 120 ms press). **kTarnished** (Tarnished mode, no Elden Ring menu, cutscene or hidden HUD -- not at a grace,
  where Snake must never take over): Start and View both like that Start (no iDroid). **kGuestMenu** (MGSV's menu): neither
  reaches Elden Ring, even when the menu closes while the button is still held (it used to: a Start that closed the iDroid
  could have opened Elden Ring's menu). **kOff** (an Elden Ring menu, a load, death, Snake not playing): both as they are.
- The chord: Start and View both down before either reached anyone. Neither reaches Elden Ring or MGSV (a View hold under
  way stops short of the iDroid); held `chord_ms` together it fires once; let go early, nothing. Both stay out of every pad
  until released, whatever the mode is by then, so Elden Ring's menu never opens or closes from it. A press still undecided
  when a menu comes up is dropped the same way. Start first: `[Input] start_wait_ms` (400; was 150) leaves room for View.
- **View a bit before Start** (player report 2026-10-14: "the chord failed when View went down a bit before Start -- the
  View hold opened MGSV's iDroid first"). Live, MGSV opens its iDroid 34 ms after the hold's Start, and the player's Start
  then went to MGSV (its menu has the pad). Now a Start within `[Input] chord_window_ms` (700) of View is the chord even
  after View's hold went to MGSV: the hold's Start is cut, the real Start stays out of MGSV's pad (also in its menu), and
  MGSV's menu, if it opened, gets a Start tap `kCloseGapMs` (100 ms) after the cut that closes it (once more 700 ms later if
  it is still open); the chord fires `chord_ms` after Start as usual. View alone for 400 ms is still the iDroid, and a Start
  pressed in it later than `chord_window_ms` after View is MGSV's (it closes the iDroid). `tests/test_host_chord.inc` (the
  iDroid opened at once, a second close tap, Start 1.5 s after View). The log: `input: Start within 700 ms of View ->
  View+Start chord; the iDroid hold sent to MGSV is cut short` and `MGSV opened its menu from the cut iDroid hold anyway -> a
  Start tap closes it`. VERIFIED live 2026-10-14 (fake MGSV `--idroid`: a Start press opens the iDroid at once, one in it
  closes it, as MGSV): View, Start 550 ms later, both held -> `View held 400 ms -> MGSV` -> `MGSV menu open` -> `cut short`
  -> 117 ms later `a Start tap closes it`, `MGSV menu closed` -> `Tarnished mode ON` 1.0 s after Start; the fake saw two
  Starts (the hold's, the close tap), never the real one. Start first and View 300 ms later: Tarnished mode, no Elden Ring
  menu. View alone 700 ms: the iDroid; a Start tap 2 s later closed it (MGSV's Start).
  **VERIFIED live with the real MGSV** (2026-10-14 combined run, three times): View, Start 550 ms later -> `View held 400 ms ->
  MGSV`, MGSV's iDroid open 33 ms later, `cut short`, the first close tap (100 ms) is too early for MGSV's opening iDroid, the
  second (700 ms later) closes it (`MGSV menu closed` 0.98 s after it opened), `Tarnished mode ON` 1.0 s after Start; Start
  first, View 300 ms later: Snake back, no menu; View alone 700 ms: the iDroid, Start closed it. The guest stops its iDroid
  retries when the hold is cut (`IDroidRequest` `kCut`: the host's Start up within 600 ms of the hold -- the ER agent's open
  edge case of an aiming Snake whose retry opened the iDroid as Tarnished mode began; unit-tested).
- When the unmasked pad (Tarnished mode) carries Start/View other than the real ones, Elden Ring gets packet numbers of our
  own range (0x20000000+), moved whenever the real packet or what it may see changes (it only re-reads a pad whose packet
  number changed).
- VERIFIED live 2026-10-13 (fake MGSV, Castleward Tunnel and Margit's arena, `press start+back 1250`): 20 toggles in two
  sessions, 0 warnings, Elden Ring's menu never opened (`menu closed`), MGSV saw no Start/Back (`pad: Back 0 Start 0`); the
  Tarnished drank a Flask of Crimson Tears (HP 286 -> 536 of 572) and back as Snake: `HP bar 50% -> 94%: healed 250 HP as
  the Tarnished -> Snake +2620 life (kHEvHeal 655)`, fake `life 3000 -> 5620`; Margit's hit on the Tarnished (`100% ->
  9%: lost 523 HP ... -> kHEvHurt 1372`); Melina's question answered with B as the Tarnished. Screenshots: the corner
  message, the Tarnished with his weapon, Snake back where he stood. The test save has no Torrent (no Spectral Steed
  Whistle among the quick items), so the whistle is UNVERIFIED.
- Snake takes over only on foot: on Torrent or on a ladder the chord is refused with "Tarnished mode: get off Torrent first,
  then View+Start" (`game::PlayerMounted`: modules+0xE8 `CS::CSChrRideModule` +0x162/+0x163 is_mounting/is_mounted,
  modules+0x78 `CS::CSPlayerLadderModule` +0x18 state, -1 none; each module's RTTI name checked before it is read;
  `pr er status` shows "the Tarnished on foot / on Torrent / on a ladder"). VERIFIED live 2026-10-13 (Stormhill, the
  player's Spectral Steed Whistle used from the inventory in Tarnished mode): +0x163 = 1 riding, the chord refused, Torrent
  ridden 12 m, dismounted, the chord -> Snake back in the grass where the Tarnished stood. The ladder state reads -1 on foot;
  on a ladder the chord is refused too (VERIFIED live 2026-10-13, Fort Haight: "Ladders" below).

**Flasks and the quick item** (`er/QuickItem.*`, `er/QuickItemLogic.*`, `er/Chord.h` the View shift, `[Flask]`; roadmap 2.10;
console `flask [use|next|slots|on|off]`; 2026-10-31). Camps killed Snake and his only heal was a grace or Tarnished mode. Now
**View held + X** makes the hidden Tarnished use the item in his selected quick slot while Snake keeps the controller, and
**View held + D-pad down** switches to his next quick item -- X and D-pad down are Elden Ring's own "use item" and "switch item".
- **The binding** (every candidate's uses: the D-pad is MGSV's weapon / item select and wheels, X its dive, A its stance, B its
  reload, Y its action (and Elden Ring's prompts, the climb), LB its call menu, RB its binoculars / first person, L3 its sprint,
  R3 the jump, Start Elden Ring's menu, View tapped its map, View held MGSV's iDroid, View + Start Tarnished mode): View is
  already "Elden Ring's button", and nothing uses View + a face button or View + the D-pad. `PadGate` (pure, `TestQuickItem`):
  in kSnake the item / cycle button going down while View is down and still undecided (before the 400 ms that make it the
  iDroid) is a shift press -- View then belongs to the shift (no map at its release, no iDroid, no chord; a Start meanwhile is a
  plain Start), every further press while View stays down counts (drink twice), and the pressed button stays out of MGSV's pad
  until released (no dive, no handgun switch). The other order (X first: MGSV's dive) is no shift; View alone works as before;
  not in Tarnished mode (Elden Ring has X itself). Keyboard: Tab (View) + Space (X) / 2 (D-pad down) should do the same (the keys
  become the pad before the gate; UNVERIFIED live). All four
  buttons are ini names (`button`, `cycle_button`, `er_use_button`, `er_cycle_button`).
- **The press** is Elden Ring's own: `InputBridge::PressErButtons` adds its use button to the masked pad for `press_ms` (150);
  Elden Ring plays the drink on the hidden Tarnished, counts the flask and heals him; forward_heals turns the heal into
  `kHEvHeal` (er/Puppet.cpp). The kinematic puppet keeps moving him meanwhile (SetPosition each frame: Snake's run is not slowed).
- **Before the press** the slot is read and refused cleanly, with a corner line and a log line: no item ("No quick item in the
  slot"); an empty flask ("Flask of Crimson Tears: empty -- rest at a grace": EquipParamGoods `goods_use_anim` 254, or a
  consumable with 0 left); an item that opens one of Elden Ring's screens, calls Torrent or takes the camera ("...: Tarnished mode
  only (View+Start)": `opme_menu_type` not 0 and not 13 -- Memory of Grace 1, the Spectral Steed Whistle 9, fingers, remembrances
  -- or the Telescope's `goods_use_anim` 4). Spirit ashes (menu type 13: the bell) are used. Not while Snake isn't playing, an
  Elden Ring screen has the pad, or he dies; riding D-Walker / D-Horse is allowed (`while_riding`). With **no EquipParamGoods
  row** for the item (the table not resolved: another regulation or build) only a flask, known by its id, is pressed; everything
  else is "Tarnished mode only" and the log says why once (`flask: no EquipParamGoods row for goods N: <table> -- only flasks are
  pressed for Snake`). Before the 0.6.0 review it was pressed as it was: Memory of Grace's question, the whistle's Torrent or the
  telescope under the hidden Tarnished (`quickitem::Classify`, `TestQuickItem`).
- **After the press** a consumable's count is watched: "Flask of Crimson Tears: 3 left" when it drops, "none left" for the last
  (the slot keeps the full flask's handle while Elden Ring swaps the entry to its empty twin in place: 1001 -> 1000), "the
  Tarnished couldn't use it now" when it didn't drop within `confirm_ms` (3000). A non-consumable (spirit ashes, a shackle) has
  nothing to count: "Spirit ashes: pressed" (since the 0.6.0 review; it said "used" even when Elden Ring refused the summon; 0.7.0
  watches what came of spirit ashes, below). A
  switch is checked 0.4 s later and pressed once more when the slot didn't move (live: the first D-pad down after a while
  switched nothing -- Elden Ring's Auto HUD, fixed in 0.7.0 below --, also the Tarnished's own in Tarnished mode; the next ones did -- and with both games 2026-11-01 every one of
  six switches took the second press), then "Quick item: Flask of Cerulean Tears (3)" / "(empty -- rest at a grace)". Not while a
  use is being followed: the retry would switch the item under the tracker, which then said "couldn't use it now" for a drunk
  flask. Elden Ring-only presses keep a deadline per button (`er/ErPresses.h`, tested): one slot used to hold them, so the cycle
  retry landing on a View + X cut the X to under a frame.
- **One press after a pause** (0.7.0; `[Flask] wake_hud`, `AutoHudHidden`): the switch "ignored" after a while was Elden Ring's
  "Auto" HUD (the player's save: GameSettings `hud_type` 2, [[GameDataMan]+0x58]+9): out of combat it fades the equipment slots
  out, and its first D-pad down then only shows them again -- the item stays (live 2026-11-03: the Tarnished's own press in
  Tarnished mode too). With `hud_type` 2 and the slots hidden (CSFeMan's Auto-HUD "shown" bytes +0x4EB8, entries 5-10 all 0; RTTI
  `CS::CSFeManImp` checked), View + D-pad down sends a 70 ms wake press and the switch 170 ms later; the 0.4 s retry stays for
  anything else. Live (fake MGSV, the slots faded): `flask: View + D-pad down -> Elden Ring's Auto HUD has its equipment slots
  hidden: a wake press, the switch 170 ms later`, `flask: the switch press after the wake (0x0002)` 183 ms later, slot 0 -> 1
  with no retry. `wake_hud = false` is the old way. Review 0.7.0: the switch after a wake re-reads the slot first (`flask: the
  wake press switched by itself (slot A -> B) -- no second press`: never two switches for one press), and a press made while
  another is still held for Elden Ring (or a wake's switch is still to come) is queued behind it, released 60 ms in between (up to
  3; `queued behind the switch still pressed (N waiting)`): `ErPresses` only raises a deadline, so two quick presses used to merge
  into one switch. Live with both games (0.7.0 integration, the Auto HUD faded after 12 s idle): `a wake press, the switch 170 ms
  later` -> `the switch press after the wake` 184 ms later -> `the quick item is now slot 1: Flask of Cerulean Tears`; two presses
  ~0.35 s apart: the second `queued behind the switch still pressed (1 waiting)`, sent 216 ms after the first, slot 1 -> 0 -> 1 with
  no retry.
- **Spirit ashes** (0.7.0; `quickitem::SpiritCheck`, `TestQuickItem`): pressed as before, then watched for 4.5 s: CSFeMan's
  summoned-spirit count (FrontEndViewValues +0x4D58 at CSFeMan +0x80, RTTI checked) going up or a new non-hostile character within
  12 m of the Tarnished -> "Spirit ashes: summoned -- they fight beside Snake" (they rise at the hidden Tarnished, where Snake
  stands); neither -> "Spirit ashes: can't be summoned here (no summoning icon, or not enough FP / HP)" (warn) and `flask: spirit
  ashes -- no spirit N ms after the press ... Elden Ring refused the summon here`. `pr er flask` counts both. A death, a load or
  Snake stopping (Tarnished mode) ends the watch with no verdict (review 0.7.0: its "refused" came over a death). UNVERIFIED live:
  the player's save has neither the Spirit Calling Bell nor ashes (Renna gives the bell: an NPC talk, not done); offline tests
  only -- the quick slots hold the two flasks only, so there is nothing to press (`flask slots`).
- **Layout** (fromsoftware-rs `cs/player_game_data.rs`; no code address -- each object's RTTI name and the inventory pointer are
  checked on every read, any failure refuses with the reason and presses nothing): PlayerIns +0x580 PlayerGameData, +0x2B0
  EquipGameData (+0x158 EquipInventoryData, +0x288 EquipItemData: +0x8 ten quick slots {u32 handle 0xB0000000 | goods id, i32
  inventory index}, +0x98 the inventory pointer == EquipGameData +0x158, +0xA0 the selected slot); the inventory index is a key
  item below the key capacity (+0x1C; entries through the accessor at +0x50) or a normal one at index - capacity (+0x10, 0x18
  bytes: handle, item id 0x40000000 | goods id, quantity). Live on 1.17.2: slot 0 handle 0xB00003E9, index 397, key capacity 384
  -> normal entry 13, Flask of Crimson Tears (1001) x4; slot 1 Cerulean (1051) x1. EquipParamGoods row size 0xB0 (SoloParam
  index 3): `goods_use_anim` +0x42, `opme_menu_type` +0x43, `is_consume` +0x48 bit 7 (regulation rows checked offline with
  `tools/re/er_params.py`: 1001 anim 10 consume 1, 1000 anim 254, 130 menu 9, 115 menu 1, 2040 anim 4, 200000 menu 13).
- **VERIFIED live** 2026-10-31 (Elden Ring 1.17.2 alone, fake MGSV with `!life 3000 0`, Stormveil then the Church of Elleh;
  presses through the virtual pad as View, then View + X 120 ms later): `flask: View + X -> the hidden Tarnished presses his item
  button` -> 0.9 s later `Elden Ring healed 250 HP (37% of 680) -> Snake +2204 life (kHEvHeal 551)` -> fake `life 3000 -> 5204`,
  `Flask of Crimson Tears used 917 ms after the press, 3 left`, the corner "Flask of Crimson Tears: 3 left" and Elden Ring's own
  quick slot showing 3; all four drunk, the fifth refused ("empty -- rest at a grace", HP unchanged); drinking while walking:
  the puppet moved 0.3-0.5 m every 0.3 s throughout the drink; the Cerulean flask (FP) used to 0; the switch both ways (one
  needed the second press). Frames: during the drink Elden Ring's flask shows at the hidden Tarnished's hand -- at Snake's
  chest, glowing red -- and nothing else of the Tarnished. MGSV's pad never carried X, View or D-pad down (fake `presses` none).
  Not tried: spirit ashes, a refused screen item (this save's quick slots hold the two flasks only), the real MGSV,
  `motion = root_motion` (Elden Ring's own movement: the drink would hold him).
- **VERIFIED live with both games** (0.6.0, Stormveil's ladder room, the player's save): a drink right after a 15.5 m/s fall (42%)
  -> `Elden Ring healed 250 HP (37% of 680) -> Snake +2868 life (kHEvHeal 717)`, Snake 4570 -> 7438 of 7801 1.5 s after the press
  (before MGSV's regeneration starts); the count down to 0, the fifth refused (`empty: refused`); ~5 m walked during one drink; the
  frames: Elden Ring's flask in the air at Snake's chest, the Tarnished never. MGSV never dived (X stayed the shift's).

**Ladders** (`er/Ladder.h`, `[Input] ladder_prompts` 5000 "Climb" / 5010 "Descend", `ladder_press_delay_ms` 100, `ladder_wait_ms`
1500, `ladder_settle_ms` 300; `[Ladder]`; roadmap 1.7; protocol v15 `kHEvClimb` / `kGuestClimbing`). Elden Ring climbs the ladder
with the hidden Tarnished -- its ladder state moves him exactly, and Snake's kinematic body couldn't -- for as long as its own
ladder state says he is on one: `CSPlayerLadderModule` (modules+0x78, RTTI checked) +0x18, fromsoftware-rs `LadderState`: -1
none, 0 StartBottom, 1 StartTop, 2/3 up, 4/5 down, 6 EndTop, 7 EndBottom, 8/9 idle on it, 14-20 the slide down (`game::LadderState`,
every change logged as `ER ladder state a -> b (Name)`). **Snake climbs it himself** (since 2026-10-31; `[Ladder] snake = on`, the default):
the Tarnished stays hidden throughout, MGSV plays Snake's own ladder climb, and Snake's meshes are drawn where the Tarnished is
on the ladder, facing it. Before (and with `snake = off`) the Tarnished showed and climbed, Snake came back at the end.
- Snake's Y on a ladder prompt steps the puppet aside (`Puppet::Hold::kLadder`): Elden Ring has the whole pad (the left stick
  climbs, B/X as Elden Ring has them) and its own ladder camera (it frames the climb from below going up, from above going down);
  the corner says "Ladder: left stick climbs". No interaction is started for it (its 3.5 s "menu" would only have kept the chord away).
- **Snake on the ladder** (`Puppet::SetClimbVisual` / `SetClimbAnchor` / `ClimbVisual`): while the hand-over holds (and Snake
  isn't in Tarnished mode, and MGSV is ready) the puppet's stop leaves the Tarnished hidden (render bit + base transparency, as
  the puppet hides him, re-asserted every frame), and the renderer draws Snake at a climb anchor the host computes after Elden
  Ring's physics every frame (`Host::UpdateClimbAnchor`, OnCamera and PostPhysics): his root at the Tarnished's feet
  (+ `[Ladder] offset` toward / up / right, 0 by default), turned so that Snake's MGSV facing is the Tarnished's (`ChooseDelta`:
  he faces the ladder whatever way MGSV turns him). The first 0.3 s ease from where the puppet had him (a "Descend" moves the
  Tarnished up to ~1 m first, below). His buddy's draws are left out meanwhile (they would hang in the air beside him), and so
  is everything else MGSV draws but Snake; MGSV's HUD layer stays (it is still Snake's screen: `Hud` counts the climb as the
  puppet's). Elden Ring's light-set learning, which samples the Tarnished on screen, waits. A death on the ladder keeps him
  hidden as any death does (Snake fades at the ladder); Tarnished mode on the ladder (the chord) shows the Tarnished again.
  `snake = auto`: the same while MGSV plays the climb (`kGuestClimbing` within `guest_wait_ms` 1500 of the first `kHEvClimb`),
  else the Tarnished shows and climbs (logged `ladder: MGSV hasn't played Snake's climb ... the Tarnished climbs instead`).
- **kHEvClimb** (`ClimbPace`, pure, `TestClimbPace`): `a = 1` when the climb starts (StartBottom / StartTop), `b` = 0 up / 1 down,
  `v0[0]` = its vertical speed (m/s): Elden Ring's climb moves him rung by rung (0.76-1.72 m/s in 0.3 s windows, live), so the
  Tarnished's own speed is eased over 0.8 s and, until that has 0.5 s of climbing in it, `[Ladder] climb_speed` (1.2) or the
  last moving pace is sent. `a = 1` again when the climb turns (up / down / **still**: a stop on the rungs that lasts 0.3 s --
  Elden Ring's climb passes through its idle states between two pushes of the stick -- sends speed 0 with the last direction)
  or its pace changes by more than half (at most once a second), never more than once in 0.2 s; `a = 0` once when the hand-over
  ends (off it, a load, a death, a press Elden Ring refused). Speed exactly 0 means "still on the rungs" (the guest holds MGSV's
  climber still since 0.6.0); a moving climb never sends 0 -- with no speed to tell it sends -1, "unknown" (MGSV's own pace). An
  origin shift moves the last height the speed is measured from (`ClimbPace::Shift`). `kGuestClimbing` counts as MGSV playing
  *this* climb only once it rose after the hand-over began (the last climb's flag can still be up while MGSV takes Snake home). Logged: `ladder: kHEvClimb a=1 b=0 (up at 1.20 m/s; ER ladder
  state 0 StartBottom) -- MGSV plays Snake's climb`, `ladder: MGSV plays Snake's climb (kGuestClimbing) 33 ms after kHEvClimb`,
  `ladder: kHEvClimb a=0 (the climb is over; N events)`; `pr er status` / `pr er ladder`: Snake drawn on N climbs, MGSV played N,
  kHEvClimb events, the pace.
- **"Descend" from Elden Ring's own place** (`AlignForDescent`, `[Ladder] descend_prompt` 5010, `top_stand` 0.55): Elden Ring
  starts a descent only from where its capsule stands on the landing, ~0.5 m along the ladder's forward (its matrix's third row,
  `GeomProbe::Asset::fwd`) from its axis, facing the hole. Snake's body can stand closer -- on the ladder's top between its
  posts. Found live 2026-10-31 (Stormveil, AEG219_067_4000, a 14.5 m ladder): there the puppet stepped aside, Elden Ring's capsule
  rose 0.33 m onto the posts, the prompt went, and the press was refused (twice, `no climb 1500 ms after the press`); put by hand
  at Elden Ring's own end-of-climb spot and turned to the hole, the same press went down at once. So on a "Descend" press the
  nearest ladder asset going down within 2.5 m is looked up and the Tarnished is put there (on the ground a ray finds within
  0.6 m of Snake's height) facing the hole, before the puppet lets go of him: `ladder: "Descend" -- the Tarnished put where
  Elden Ring starts it: 1.18 m from Snake, 0.55 m along AEG219_067_4000's forward, facing the hole` -> `StartTop` 500 ms after
  the press. No ladder found (or no landing there): he stays where Snake stands, as before.
- **Live 2026-10-31** (fake MGSV humanoid, Elden Ring 1.17.2 headless 4K, Stormveil's AEG219_067_4000 near the ladder room by
  the Rampart Tower): up `+14.5 m` in 16.7 s (StartBottom 500 ms after the press, `MGSV plays Snake's climb (kGuestClimbing) 33
  ms after kHEvClimb`, 8 kHEvClimb events with the shots' pauses, 28 before the pace was eased and the still states debounced),
  `Snake was drawn on it (MGSV played the climb)`, `puppet ON` at the top; down `-14.5 m` in 17.6 s after the alignment. Frames
  (`hud shot`) at three points of each climb: Snake's model on the ladder, the Tarnished nowhere, MGSV's HUD layer, Elden Ring's
  ladder camera (going up: behind and below him; going down in the tower's shaft: close over his head, as it frames the
  Tarnished there). The test model hung ~0.4 m left of the ladder's middle going up -- whether MGSV's own climb pose does too is
  for `[Ladder] offset` once a recording of the real climb exists.
- **Live 2026-10-31 with the real MGSV** (docs/STATUS.md "0.5.0-alpha"): Stormveil's AEG219_067_4000 up 15.4 s / down 16.5 s and
  Fort Haight's AEG030_902_2001 up +18.9 m in 17.8 s / down -18.9 m in 18.8 s; `kGuestClimbing` 33-34 ms after `kHEvClimb`,
  MGSV on its ladder 0.13-0.5 s after Y, 1-2 loops of MGSV's 8 m ladder per climb (~1 s each where his pose loses the rungs),
  the Tarnished never visible in the frames, Snake in control at both ends. Elden Ring moved its origin 32 m mid-climb at Fort
  Haight: the start (`g_ladderFrom`) and the climb anchor's blend now follow origin shifts (`Host::OnWorldShift`; the summary
  had read "-13.1 m up"). `[Ladder] offset` stays 0: the frames showed his hands on the rails.
- **The puppet's start after a ladder** (0.6.0, review 2026-10-31; `Puppet::Start`, `er/StagedFacing.h`): Elden Ring's climb is over
  at once, MGSV takes Snake home 0.2-2 s later, so the puppet starts while the guest still has him at its airport ladder. Its
  delta used to be taken from his facing there (the ladder's yaw): once he was home Snake, his motion and the camera turned by
  (home - ladder) yaw, a different angle after every climb. Now the delta comes from his facing before the climb (the last one
  while he wasn't staged) and the Tarnished's, and is taken again from where MGSV puts him once `kGuestClimbing` drops (3 s at
  most); meanwhile his body is drawn the Tarnished's way. Live: `puppet: MGSV still plays Snake's ladder climb (staged at the
  airport) -- drawn the Tarnished's way, delta from his facing before it (known)` -> `MGSV has Snake home -- delta 2.280 -> 2.280
  rad` 0.22 s later (no turn), after both climbs.
- **Live 2026-10-31 evening with the real MGSV, 0.6.0** (docs/STATUS.md): Stormveil's AEG219_067_4000 up `+14.6 m` in 18.9 s with a 5 s
  stop on the rungs (`kHEvClimb a=1 b=0 (still on it at 0.00 m/s ...)` -> MGSV held still), down `-14.5 m` in 14.0 s; MGSV moved along
  its own ladder 4 times each way, 0 loops (frames: hands on the rungs at every point looked at).
- **The press reaches Elden Ring only after the step-aside** (`InputBridge::SetPrompt(.., deferred)` + `PressForEr`): the
  bridge keeps the real Y out of both games, and Elden Ring alone gets a 150 ms Y `ladder_press_delay_ms` later, with the
  Tarnished in its hands. Found live: pressed while the puppet still held him, "Descend" at the top never started (the prompt
  went, no ladder state for 1.5 s; alone the Tarnished starts StartTop 0.5 s after his Y), while "Climb" at the bottom did.
- On the ladder Start/View go through the Tarnished-mode gate (`PadGate` kTarnished): View + Start there is the chord (Tarnished
  mode from then on, counted from the ladder for the HP carried over), never Elden Ring's menu and map together (found live: the
  chord on a ladder opened Elden Ring's main menu). Tarnished mode's own chord back to Snake is refused on a ladder ("get off the
  ladder first").
- **The end, without a dead stick** (0.6.0; `LadderEndedForGood`, `TestLadder`; console `ladder early on|off` for before/after): Elden
  Ring's none right after EndTop / EndBottom is the climb's real end -- `kHEvClimb a=0` goes out then (`ladder: Elden Ring's climb is
  over (EndTop -> none) -> kHEvClimb a=0 now: MGSV takes Snake home during the 300 ms settle`), not with the puppet's start, so MGSV
  has him home (0.18-0.23 s live) before the settle ends; and the climbing stick is latched at EndTop / EndBottom (if still pushed
  then, until let go), not at the end, so a stick let go at the top and pushed again during the settle is a fresh one. Measured live
  2026-10-31 (Stormveil's AEG219_067_4000, +14.6 m in 13.8-14.0 s, fake MGSV with `!climbhome 200` as the real guest's way home):
  before (`early off`) -- none at 0, puppet ON +300 ms still staged, `MGSV has Snake home` +519 ms, and the stick pushed again at
  +162 ms never moved him (latched as the climbing one) within 3 s; after -- none at 0, `kHEvClimb a=0` at 0, puppet ON +317 ms with
  Snake home already (no staging line), the stick pushed at +78 ms -> Snake walking in MGSV at +347 ms (30 ms after the puppet's
  start). Held through the end it still stays latched: 0.00 m in 1.5 s pushed, 3.24 m in 0.6 s after letting go and pushing again.
  With both real games (2026-11-01, the same ladder; the guest's odometer no longer still for 300 ms after a pushed stick, GUEST.md
  22.2): up `+14.6 m` in 18.8 s with a 5 s stop at +7 m (`still on it at 0.00 m/s`, +6.991 m at both ends of the stop, frames 2.5 s
  apart the same pose), `15.5 m climbed in MGSV, 4 moved along MGSV's ladder (seamless), 0 loops`; down `-14.5 m` in 19.1 s with a
  4.5 s stop, `16.1 m ..., 4 moved ..., 0 loops`. After each: none at 0, the stick pushed at +53 / +35 ms, `back on the sandbox` +205
  / +195 ms, puppet ON +301 / +317 ms, Snake walking in MGSV +522 / +440 ms (MGSV's first step once the pad is his again).
- Off the ladder for `ladder_settle_ms` (EndTop/EndBottom then none), Snake takes over where the Tarnished stands (the puppet's
  start anchors him there); what the Tarnished went through on it (an arrow, a fall) goes to Snake as in Tarnished mode (`CarryHp`,
  `kHEvHeal` / `kHEvHurt`). **The stick that climbed doesn't run Snake on** (`InputBridge::HoldLeftStickUntilReleased`): it stays
  out of MGSV's pad until it is let go. Found live: holding the stick up through the end, Snake ran off the tower's landing and
  fell 19 m down the ladder's shaft (92% of his life).
- **Snake walks sooner after a climb** (0.7.0; `[Input] ladder_home_settle_ms` 50; `Ladder::Input` endedForGood / guestHome,
  `TestChord`): of the ~0.5 s from the top until Snake walked, 300 ms was the settle, there so that one of Elden Ring's state
  flickers on the ladder never hands Snake over mid-climb. Once the climb is over for good (`LadderEndedForGood`: none right after
  EndTop / EndBottom, `kHEvClimb a=0` out) *and* MGSV has Snake home (the guest neither kGuestClimbing nor kGuestMantling), 50 ms
  is enough: the puppet starts as soon as MGSV is back on the sandbox (+195-205 ms in the 2026-11-01 runs) instead of at +301-317
  ms, ~0.1 s sooner. Any other end (a death, the chord, Elden Ring's none without EndTop / EndBottom) keeps `ladder_settle_ms`.
  Offline tests: done at 50 ms with both conditions, not before 300 ms with either missing. **Live with both games** (0.7.0
  integration, Stormveil's AEG219_067_4000, Snake set at its foot and walked into it: `prompt 5000`, Y; MGSV played the climb):
  two climbs, each down to the bottom -- Elden Ring's climb over (EndBottom -> none) at 0, `climb: back on the sandbox` +206 /
  +207 ms, `puppet ON` +233 / +234 ms (2026-11-01: +301 / +317 ms); the second with the stick let go and pushed again at EndBottom:
  `the player's stick 39 ms after the move -- Snake's motion counts again (MGSV's HUD thaws at 150 ms)` and Snake walked 4 m in the
  next 3 s. A stick held down through the end stays out of MGSV's pad until let go (as designed: no run off a landing).
- **Snake starts on ground** (`kinematic::SupportedStart`, kinematic motion, any puppet start): Elden Ring's capsule can stand on
  a rim with its centre over a drop, the kinematic body couldn't (then one ray under its centre; now five, "Collision filters and
  the ground under Snake" below: one probe on a rim is still no support). Live, a climb's end left the Tarnished
  on the shaft's rim -> `puppet: no ground under the Tarnished's centre at (3.06, 3.25, -1.54) (a rim, a ladder's top) -> Snake
  starts 0.60 m away on ground`; that first point was the ladder's own top, so the body's four radius points must stand on
  ground too now (unit-tested; the rim end didn't come again in three more climbs: those ended 1.5-2.7 m from the shaft).
  Holding the stick up through the end of a climb (one 6 s push): the Tarnished walked 2.7 m off the ladder, Snake took over
  and moved 0.11 m in the 4 s the stick stayed pushed (latched).
- **Ground under a gap** (player report 2026-10-14: back to Snake at the top of a wooden scaffold the Tarnished had climbed,
  he fell through it at once and died; likely cause: his ground was one ray at his axis, and between two boards -- Elden
  Ring's floors have 4-10 cm seams, see `SolidHeadroom` -- it finds nothing). The jump branch's own fix (seam probes 7 cm
  around the axis and `ground_ring` probes, ground needed on both sides) was merged into the body's five ground probes
  (`BodyGround`, "Collision filters and the ground under Snake" below), which cover both cases: a seam and a 20 cm gap
  under his axis hold him, standing, walking across, falling or landing from a jump (`TestScaffold`, kept; with
  `ground_probes = false` he falls through both, 10.8 m/s, as the player died). One ground model: no `ground_ring` key. At
  the scaffold's end he goes over once fewer than two probes are on it (his axis up to 0.8 x radius past the edge), as at any
  edge; a start there is moved in (`SupportedStart`).
- A press Elden Ring never turns into a climb (`ladder_wait_ms`) gives Snake the pad back (`ladder: no climb ...`); a load or a
  death on the way ends it. A ladder state while Snake plays (a press we didn't see, e.g. Elden Ring's keyboard) starts the
  hand-over too. With the ladder module not the verified one: `ladders: ... the ladder hand-over is off` (Tarnished mode climbs).
- `pr er status`: `ladder idle|waiting for the climb|climbing|stepping off (state N Name; C climbs, R without a climb)`;
  `geom ladders [radius]` lists the ladder assets of the loaded blocks (AssetEnvironmentGeometryParam +0x3C bit 0 `isLadder`,
  168 rows in this regulation; nameless entries of the instance arrays are not ladders).
- VERIFIED live 2026-10-13 morning **with the real MGSV** (combined run, Fort Haight's tower again, reached the same way with
  the puppet off): Y on "Climb" (prompt 5000 shown walking up to it) -> `the press goes to Elden Ring now, 100 ms after it` ->
  `StartBottom` 500 ms after the press -> `the Tarnished is off it after 17.6 s, +19.1 m up` -> `puppet ON`, "Snake is in control"
  (screenshot), three times. Down: "Descend" (5010) showed once Snake faced the hole from where the Tarnished got off (turned in
  place with MGSV's own Warp rotation for the script; a person turns him with the stick) -> `StartTop` 501 ms after the press ->
  `EndBottom` -> `off it after 17.9 s, -18.8 m` -> Snake at the bottom. Twice the script's stick walked Snake on into the hole
  from the top (no prompt on the way): a 19.2 m/s landing, 92% of his life, as the Tarnished would fall -- README tells players
  to face the hole and press Y instead.
- VERIFIED live 2026-10-13 (fake MGSV, Fort Haight's tower, ladder AEG030_902_2001, 19 m: around the five graces the save has
  near the route -- Castleward Tunnel, Stormhill Shack, Gatefront, Church of Elleh, Agheel Lake North -- `geom ladders` found no
  ladder; reached with `call setpos` hops east from Gatefront): Snake's Y on "Climb" -> `ladder: the press goes to Elden Ring
  now, 101 ms after it` -> `StartBottom` 484 ms after the press -> up (2/3, idle 9 between) -> `EndTop` -> none -> `the Tarnished
  is off it after 17.5 s, +18.9 m up` -> `puppet ON` at the top, the stick held up 3 s more: Snake didn't move; "Descend" at the
  top -> `StartTop` 501 ms after the press -> down (4/5) -> `EndBottom` -> Snake at the bottom (screenshots: the Tarnished on the
  ladder with the corner message, climbing down). The re-backlog's ChrCtrl flags: +0xE8..+0xEE read `60 00 00 00 00 00 00` on foot and
  all through two of Elden Ring's climbs (sampled every 1.5 s; +0xEF was 0x3f in one climb and 0 in the other and on foot): none
  of +0xE9 (fromsoftware-rs `disable_move`), +0xEB, +0xEC, +0xED marks a ladder in this build -- and with the puppet off on a
  ladder they matter to neither motion. Tarnished mode on a ladder (the second run): the chord mid-climb -> `Tarnished mode ON
  (View+Start on a ladder)`, no Elden Ring menu; the chord again on the ladder -> refused, "Tarnished mode: get off the ladder
  first, then View+Start" (screenshot); off at the top -> `Tarnished mode keeps him`; the chord on foot -> Snake back.

**Climb up and vault** (`er/Mantle.*`, `[Mantle]`, protocol v15 `kHEvMantle` / `kGuestMantling`; console `mantle`; 2026-10-31). MGSV
climbs onto a ledge with Y (its action button) and vaults low fences; its flat airport has no such walls where Elden Ring has
them, so the host finds the ledge in Elden Ring's world, tells the guest which move to play and for how long, and carries the
kinematic puppet up and over while MGSV plays it.
- **The ledge** (`PlanMantle`, pure, `tests/test_host_mantle.inc` `TestMantle`: 12 lanes of a synthetic world and the path checked
  clear of it at every step): a face within `reach` (0.6 m) of his body at knee, waist or chest height straight ahead; its top
  just past the face, `min_height`-`max_height` (0.5-1.8 m) above his feet (looked for from 2.15 m down, and under anything
  higher there: a ceiling over the top, a beam over the face; lower: a step he walks onto; higher: a wall); room to rise along
  the face (`climb_room` 1 m over the top: his axis from the feet, his sides, back and front from just above the top -- live,
  Gatefront's rocks lean toward him and are 0.1 m higher at his side, so knee height read "no room"). Then:
  - **vault over** (b = 1): a top at most `vault_max_depth` (0.7 m) deep (a fence, a low wall), its highest point on the way over
    at most `vault_max_height` (1.35 m since 0.6.0: Limgrave's wooden rail fences stand 1.26 m -- AEG700_068 by the Church of
    Elleh, live 2026-10-31: `none -- nothing to stand on up there (face 0.20 m ahead, top 1.26 m up)` with 1.2, `vault over -- fits
    (face 0.26 m ahead, top 1.26 m up, 0.15 m deep, drop -0.04 m, 1.42 s)` with 1.35; lane 170 of `TestMantle`; with both real games
    2026-11-01 AEG700_068_1003 by Y: `vault over a 1.10 m obstacle ... -> kHEvMantle a=110 b=1 1.40 s`, MGSV 16 ms later, over in
    1.40 s; running into it: `1.16 m ... (running into it) -> kHEvMantle a=116 b=1 1.41 s`; AEG700_069_1003 measured 1.31 m from the
    west and ~1.45 m from the east, above `run_into_max`: running into it broke it), ground behind it
    no more than `vault_max_drop` (1.6 m) below his feet (more: "a drop behind
    it"), the whole body standing there (five probes) with room for his height, and nothing in the way 0.3-0.9 m over its crest.
    Path: up beside it to the crest + 0.25 m (35% of the time), over (70%), down to the far ground; `vault_base_s` 1.1 +
    `vault_per_m` 0.25 per metre (+ 0.2 s per metre of drop): MGSV's own vault, from the request, is on top at ~0.8 s and down
    at ~1.35 s (live 2026-10-31; 0.55 had Elden Ring's path done while MGSV still reached for the top).
  - **climb up** (b = 0): a top to stand on `radius` + 0.1 m past the face (its height within `maxSlope` x that of the edge's, the
    body's five probes on it, room for his standing height), nothing in the way over the edge. Path: straight up beside the face
    to the top + 5 cm (55%), then over onto it; `climb_base_s` 1.8 + `climb_per_m` 0.2 per metre (2.0 s for 1 m, 2.14 s for 1.7 m;
    0.6.0: the guest plays MGSV's full climb-up onto a crate / drum, ~2 s from its Y at any height -- live, 0.75 + 0.45/m ended Elden
    Ring's path 0.6-1.0 s before MGSV's move; now `climb up done` came 8 ms after MGSV's `move over`).
  - a thin obstacle where climbing isn't possible is vaulted even when only climbs are wanted; anything else is refused with a
    reason ("too high to climb (a wall)", "no room above him to climb", "nothing to stand on up there", "a drop behind it", ...).
- **When**: **Y** (rising edge on the real pad, `[Mantle] y`) with none of Elden Ring's prompts up now or within
  `prompt_quiet_ms` (400): at a grace, a door or an item the Y stays Elden Ring's (`InputBridge::SetPrompt`). Facing: Snake's.
  Nothing in front: nothing happens (the Y was MGSV's anyway); a ledge that doesn't fit: logged `puppet: Y at a ledge 0.25 m ahead
  -- no climb: <why>`. **Running into it** (`run_into = low`, the default: asking `run_speed` 3 m/s or more and stopped by it for
  `run_blocked_s` 0.12 s): a ledge or fence up to `run_into_max` 1.3 m (1.1 before 0.6.0: below the rail fences, which broke
  instead) is climbed / vaulted without Y (`all`: any that fits;
  `off`: Y only). Never riding, prone, aiming (first person, binoculars, a throw), in CQC, the box, a dive or a jump, in the air,
  in MGSV's menu, nor with the buddy within `buddy_clear` (1.3 m: Y mounts D-Walker / D-Horse in MGSV). With C4 in hand the
  climb's Y no longer sets one off (below).
- **Y is the climb's alone** (`LedgeAhead` every 100 ms in Snake's facing, with TryMantleY's refusals; `InputBridge::SetLedge`):
  while a ledge that fits is in front of him, a fresh Y stays out of MGSV's pad until released. Since 0.6.0 the climb starts from
  exactly those presses (`InputBridge::LedgePresses` rising), not from the puppet's own look at the button: the two could disagree
  within the 100 ms (review 2026-10-31: a ledge just in reach let Y reach MGSV *and* climb; a press and release between two frames
  was missed). A kept press that finds no climb a frame later goes to MGSV after all (`InputBridge::PressForMgsv`, 150 ms; logged
  `puppet: Y kept from MGSV for a climb that no longer fits`). Found live 2026-10-31 with the
  real MGSV: D-Walker follows ~2.5 m away (outside `buddy_clear`), MGSV's Y reached it first and mounted it at a Stormveil fence;
  with C4 in hand MGSV's Y also set one off. Now neither happens (`pr er mantle`: `a ledge ahead now: yes (N Y presses kept from
  MGSV)`); live: Y onto a 1.09 m wall with D-Walker 2.36 m away in MGSV -> climbed, not mounted. Within `buddy_clear` Y still
  mounts.
- **The move** (`MantleRun`): `kHEvMantle` (a = height cm, b, v0[0] = the duration) goes out and Snake holds still until MGSV
  says it plays the move (`kGuestMantling`) or `wait_ms` (300) passed; then the puppet follows the path, checked against Elden
  Ring's world before it starts (never through a wall or a ceiling), with no collision of its own meanwhile; Snake's own motion,
  jumps and dives wait until it is over; at the end he stands on the ground there (a fall from there is an ordinary one). An
  Elden Ring warp on the way ends it. Logged: `puppet: Snake climbs onto a 0.68 m ledge 0.04 m ahead (Y) -> kHEvMantle a=68 b=0
  1.05 s, to (...)`, `puppet: the climb up starts 67 ms after kHEvMantle -- MGSV plays it (kGuestMantling)`, `puppet: climb up
  done at (...)`. The drawn Snake follows the puppet's root (MGSV's draws are relative to his root, as for a jump): the guest's
  move should keep his root where the body is, else the lift shows twice.
- **Drawn facing the move** (0.6.0, review 2026-10-31; `er/StagedFacing.h`, `Puppet::StagedDrawAnchor`, `RenderScene::Capture`): the
  guest plays the move at its own obstacle, facing that obstacle; drawn with the puppet's delta his body faced (the site's yaw +
  delta), sideways or backwards to the climb. While `kGuestMantling` is up the draw list is drawn with a delta of its own -- the
  plan's direction minus the list's `anchorYaw` -- eased in over 0.15 s from the way he faced and back out once MGSV has him home;
  the camera and everything mapped from around his home keep the puppet's delta, and the hidden Tarnished faces the plan's way.
  Live: frames through three climb-ups (1.71, 1.15, 0.57 m): hands on the top, a knee up, standing on it, facing the face throughout.
- **Hints** (`[Hints]`, `count` 3 per kind and session, `blocked_s` 0.6, at least 20 s apart -- live the three of a kind went out
  in three seconds at the first wall): walking into something (stopped by it while asking to move) for 0.6 s plans a climb in
  that direction once a second; what fits gets a corner hint the first few times: "R3: jump onto
  it -- Y: climb up" (a ledge he can also jump onto: up to `jump_apex` + `jump_air_step` - 0.15 m), "Y: climb up", "Y: vault over
  -- or run into it". The jump's button follows `[Jump] button`. `pr er "mantle hints reset"` shows them again.
- **[Jump] button** (`r3` default | `a` | `l3`; console `jump a`): the guest's jump is its R3 (`[Jump] Button = rs` in
  PhantomRingGuest.ini, unchanged). With `a` the host swaps A and R3 in the pad MGSV gets while Snake plays (`InputBridge`
  `SwapJump`): A jumps -- and selects in MGSV's call menu (LB) and uses in its weapon / item wheels (the Phantom Cigar), as R3 did
  -- and R3 crouches / lies down (MGSV's A). `l3`: L3 jumps, R3 sprints (MGSV's L3 is its DASH). Not the keyboard (its middle
  mouse button jumps), not in MGSV's menus.
- `pr er "mantle plan [deg]"` says what a climb would do in front of Snake (or along a heading), `mantle go` starts one as Y would,
  `mantle on|off`, `mantle run off|low|all`; `pr er status` (puppet line) counts climbs up / vaults (running, played by MGSV,
  refused) and hints. A refusal also gives how slanted the face is and where the refusing ray hit (`..., 21 deg from the face's
  normal [blocked 0.97 m up, 1.13 m ahead, 0.00 m aside]`); running into one that fits nothing logs it too (`puppet: ran into a
  1.08 m ledge 0.00 m ahead -- no climb: <why>`, the first 10, then every 50th).
- **Rougher world** (2026-10-31 evening; Gatefront's rocks, the Church of Elleh's rubble and rail fence; `TestMantle` now 17 lanes
  plus every lane at four headings with Elden Ring's ray rule -- a ray from inside a shape sees nothing of it, `MantleRayEr` -- and
  the path checked clear each time):
  - **A slanted face** (`FaceNormal`: three parallel rays 0.2 m apart across it at the height that hit nearest; `[Mantle] max_slant`
    70 for Y, `run_max_slant` 50 for running into it): a face 25 degrees or more off square is planned square to it first, along
    its normal (MGSV climbs and vaults straight on; a diagonal vault would cross a fence lengthwise), the asked direction only if
    that fails; nearly square, as asked first. Past the slant allowed (running along a wall) nothing is squared. Live: Y at a
    Church of Elleh boulder at 21 degrees -> `Snake climbs onto a 0.72 m ledge 0.17 m ahead (Y, squared up to the face)`, +0.88 m.
    Before, the 45-degree case worked in the synthetic world but 70 degrees found no face in reach (the body slides to 0.35 m
    of the wall: 1.0 m away along a 70-degree line).
  - **A rounded rock's crown**: the top is the highest of the three columns past the face (0.06, 0.15, 0.25 m), not the first.
    Live at Gatefront a boulder's 0.40 m lip read "low enough to step onto" while its crown stopped the walk -- Snake stood
    stuck against it; now `climb up -- fits (top 0.68 m up)`, Y -> on top (+0.76 m, 1.06 s).
  - **A top rising past its lip**: the landing's ground is looked for from 0.65 m over the lip (was 0.3: inside the rock, and
    Elden Ring's rays don't hit from inside -- "nothing to stand on up there").
  - **Stacked rubble, a rock still rising** (`kFarLandings` 4 x `kFarStep` 0.25 m): when the first landing has no room, too little
    top, too steep a top or something in the way, landings farther on are tried on the highest ground met so far (each at most
    0.7 m above it, never above `max_height`), with room to rise to that height, room to stand there and nothing in the way over
    everything passed. A wall past the first step higher than he climbs, a gap or a drop ends the search (lane 130: refused).
  - **Rail fences**: the face rays run every 0.2 m from 0.3 to 1.1 m (a Limgrave rail fence's two rails slipped between 0.3, 0.7
    and 1.1 m: "nothing in front to climb" at a fence the walk was stopped by), and the path's clearance rays over the crest every
    0.08 m (every 0.2 m let a 0.12 m beam over a ledge slip between two rays once rays from inside a shape stopped counting -- the
    synthetic lane 110 climbed through its beam with Elden Ring's ray rule; a latent hole, closed).
  - **Running into a fence vaults it instead of breaking it**: the bump breaker (`Explosion::Break`, "Snake bumped into it") fired
    on the first blocked frame, `run_blocked_s` before the vault could start -- live a rail fence by the Church of Elleh broke and
    Snake ran through. Now a blocked running frame first asks whether a climb / vault fits running into it, and while one does
    (0.5 s) nothing is broken. Walking into one (below `run_speed`) still breaks it, as before. UNVERIFIED live: both fence tries
    broke it walking into position, and the Tree Sentinel patrols that road (it killed Snake once; the save was put back).
- **MGSV's HUD at a climb's start** (`[Overlay] climb_hide_ms` 1600, `Hud::HideLayerFor`): the guest stages Snake at MGSV's barrier
  or wall ladder, and MGSV's last prompt there (D-Walker's ride icon, its action prompt) faded out over ~1.5 s over Elden Ring's
  frame. From each `kHEvMantle` and each climb's first `kHEvClimb` the layer is cut at once (no fade) for 1.6 s, then fades in
  as usual. Live with fake MGSV's layer: `HUD layer 0.00 ... cut at a climb's start 1 times` 0.25 s after Y, 1.00 again after
  the climb; `TestOverlay`.

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

- the character's forward axis (run direction vs `quat·(0,0,1)`): -Z in every session (0/40 votes; 2026-10-13, Elden
  Ring's own control in four stick directions: the physics quaternion's +Z 180 deg from the run each time), so -1 is
  also the value until it is learned (it was +1: harmless while SetYaw pitched the puppet, Motion below; with the yaw fixed
  it turned the hidden puppet backwards against Snake's motion -- live, with -1 it faces where Snake goes);
- the camera `+0x30` row sign (camera → player vs that row), plus the row handedness.

The ini can pin each of them.

### Motion and hits: the two RE-backlog hooks (kinematic motion is the default; hit_hook on since phase 3)

Both are **instance-vtable swaps** (`er/VptrSwap.h`): the one game object gets a private copy of its
vtable with one slot replaced; no code or `.rdata` is written. Both are installed when the puppet starts
and restored when it stops (only on the same, live object).

- `[Puppet] motion = root_motion` (off by default since the player's first session, 2026-10-12 evening: "invisible walls
  everywhere"; re-backlog E1, `er/RootMotion.cpp`): PlayerCtrl vfunc 12
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
  **Likely cause of the invisible walls, fixed 2026-10-13:** `ChrIns::SetYaw` takes *Euler angles* about Elden Ring's X,
  Y and Z axes (`0x461F60` multiplies `[0]`, `[1]`, `[2]` by 0.5 and rotates about the unit vectors at `0x1448062A0` /
  `0x1448062D0` / `0x1448064E0`, read live: (1,0,0), (0,1,0), (0,0,1)); `game::SetYaw` passed the yaw in `[0]`, so the
  puppet was *pitched* by Snake's heading (live: physics quaternion (0.47, 0, 0, 0.88) for a 56 deg "yaw", Elden Ring's own
  (0, 0.47, 0, 0.88)). Root motion turns Snake's displacement into character-local space with that quaternion, so it was
  tilted into the ground or the air by an amount that changed with the heading -- the "60-95% of what it is handed" above.
  With the yaw in `[1]` (live, fake_mgsv, Castleward Tunnel to Margit's fog wall up two flights of stairs): 23.01 of
  22.94 m asked, six flat walks in six directions 100% (two ending at a wall 74-82%), the mist's wall stopping it; down and
  up the stairs again at up to 4 m/s 39.41 of 39.17 m, never stuck; the mist step-aside and Tarnished-mode toggles with it
  installed/removed the hook cleanly. **With the real MGSV** (2026-10-13, combined run; the same 8-leg loop grace -> stairs ->
  mist -> back, twice each, Snake run by the pad's stick toward each point): root motion 88.1 of 87.9 m asked (100%), 16/16
  legs arrived, 0 unstucks; kinematic 71.6 of 96.2 m (74%: it stops at the tunnel walls and stair edges the straight legs
  graze), 14/16 arrived. Not yet played by a person, so `kinematic` stays the default; the player can try
  `pr er "puppet motion root_motion"` (and `kinematic` to go back), live.
  **The 10-minute comparison** (2026-10-13 night, roadmap 2.15; fake MGSV with its stick camera-relative like MGSV's, 5 m/s at
  full stick, the virtual pad's stick steering it at 300 ms steps straight at each point; 12 points around the Stormhill Shack
  grace, re-read against the shack every step since Elden Ring shifts its origin mid-walk: through the shack's ruins twice, past
  its crates and palisade, along the ledge south of it, up the north slope, down the west one; each run 10 minutes from the
  grace; a leg is "stuck" after 1.8 s without moving; counters from `pr er status`, reset by `puppet motion`):

  | | kinematic | root_motion |
  |---|---|---|
  | legs arrived | 72 of 125 (58%) | 80 of 103 (78%) |
  | legs stuck / timed out | 53 / 0 | 22 / 1 |
  | longest run of legs without arriving | 13 (trapped ~35 s) | 5 |
  | distance moved of asked | 1543 of 2258 m (68.4%) | 1728 of 2288 m (75.5%) |
  | the leg past the shack's corner | 2 of 10 arrived | 8 of 8 |
  | unsticks | 2 | 0 (not used) |
  | wall contacts (onsets / blocked frames) | 576 / 10347 | 1005 / 8664 (the proxy's "moved < 30%" test is noisier) |
  | object-only breakers sent at bumps | 542 | 0 (Elden Ring's proxy breaks what it runs into) |
  | falls, re-anchors, root-motion fallbacks | 0, 0, - | 0, 0, 0 |
  | warnings | 2 (`odometer jumped 8.8 / 13.1 m`: the Python fake stalled) | 0 |

  Both fail the same two legs through the shack's walls (from the grace into its ruins and back): a straight line into a wall
  stops either; root motion slides round corners and edges where the kinematic capsule catches (the shack's corner, the
  palisade). The number of legs differs because a stuck leg ends in ~2.5 s and an arrival takes ~5.5 s. Before the SetYaw fix
  root motion lost 5-40% of every step; now it moves more of what Snake asks on rough ground too. **Recommendation: root_motion
  as the default** -- not changed: the player played kinematic and liked it, and root motion hasn't been played by a person
  since the fix (it was "invisible walls everywhere" before it); try `pr er "puppet motion root_motion"` for a session first.
  Kinematic stays the fallback (it needs no hook and is what fall damage was tuned on).
  **With the real MGSV** (2026-10-13 morning, combined run; the same script and loop, 4 minutes each, MGSV's own Snake run by the
  virtual pad): kinematic 19 of 30 legs arrived (63%), 438.6 of 598.4 m (73.3%), 11 stuck, 0 unsticks, 103 wall onsets; root
  motion 16 of 25 (64%), 403.7 of 616.1 m (65.5%), 3 stuck + 6 timed out, 371 wall onsets, 0 breakers; 0 warnings, 0 falls, 0
  root-motion fallbacks in both. With MGSV's real acceleration and turning neither is clearly ahead on this loop (the fake's 78%
  vs 58% doesn't carry over), so **kinematic stays the default**; root motion remains the one to try.
- `[Puppet] motion = kinematic` (the old way, `er/Kinematic.cpp`): our own capsule against `cs_phys_world_cast_ray`. It
  now probes walls at knee, waist (0.9 m) and head (1.6 m) height as well as the chest, and **unsticks**
  (`[Puppet] unstick`, `unstick_seconds` 0.5): every third frame `Enclosed()` counts which of 8 directions meet geometry
  within the body's radius + 0.3 m; boxed in (7-8) for `unstick_seconds`, the puppet goes back to the newest place it
  stood free (kept every 0.25 s, the last 2 s, at least 0.5 m away; `kinematic::Unstick`, tested). Its body bumping into
  something (a blocked step) also sends the object-only breaker there (below), at most every 400 ms. **The default**
  (`motion = kinematic`): it plays well (the player's 2026-10-12 evening session).
  **Low ceilings** (`[Puppet] ceiling`, `height_stand` 1.8 / `height_crouch` 1.2 / `height_prone` 0.5, `height_ride` 0 = no
  check while riding; `kinematic::KinematicParams::bodyHeight`, 2026-10-14): player report, Snake walked into the low space
  under the Roundtable Hold's basement stairs and was stuck there (`walls 59 (1353 frames), unstuck 0`). The wall rays are
  horizontal, so a sloped underside 1.7 m up never met the head ray (1.6 m): he walked in with his head in the stairs. Now
  his height comes from MGSV's stance (GuestState `kGuestCrouching` / `kGuestProne` / `stance`), and:
  - **free** (nothing lower than him above his axis), a move that would end under a ceiling lower than his height is refused
    (`Headroom`: a ray up from 0.25 m above the feet, at his axis and 0.2 m ahead) -- however long he pushes;
  - **already under one** (he stood up beneath it, Elden Ring or an unstick put him there), a move that is no lower than where
    he is goes (walking out works), and if every try is refused for `kCeilingEscapeS` (0.75 s) the check stands aside until he
    stands free (`ceilingEscape`; logged `the ceiling check stands aside`): the way out from under a staircase can dip;
  - `SolidHeadroom`: Elden Ring's stairs are tread plates with 4-10 cm seams between them (measured with `call ray`); a ray
    through a seam reads "no ceiling", so a ray that finds nothing is checked again 7 cm around it;
  - the wall rays above his height are left out (crouched, he goes under a 1.3 m beam; prone, the knee ray only).
  Refusals count as walls (and `ceilings N` in `pr er status`; logged the first 5 times). Tested in the synthetic world
  (`tests/test_host_motion.inc`: a staircase entered from the side and from beneath, a seamed staircase, a table, a beam,
  already under a table, a low passage with a dip on the way out, up the stairs on top).
  **VERIFIED live 2026-10-14 with the real MGSV** under the Roundtable Hold's basement stairs (the player's: a wooden staircase
  down into the storage basement, 10 m below the hall; the underside comes down from 4.2 m to the floor over 4 m): standing,
  in four lanes under the stairs, Snake stopped where 1.85-2.01 m of room was left; crouched where 1.32-1.57 m was; standing up
  there and walking out worked in every lane; two corner pushes ended in `puppet: stuck at ... back to ... where he stood free`.
  Before the last two fixes the same test let a crouched Snake creep seam by seam under a 0.57 m ceiling, where a stringer
  0.59 m up between him and the higher treads held him for good (every way out read "lower than here"), and a free one take
  moves that ended under a 1.6 m underside (the old rule compared with the room 0.2 m ahead). Side effect: he stops ~0.2 m
  short of low overhangs and table edges (his ahead probe).
  **Stalls** (`[Puppet] stall_seconds` 2.5, `kinematic::Unstick::Stall`): the session's stall was never "boxed in" (7 of 8
  directions), so the old unstick never fired. Now Snake asking to move and being refused (moved < 25% of what he asked) in
  every direction he tries -- three or more of eight sectors -- for 2.5 s of trying (pauses up to 4 s don't end it, 0.25 s of
  real moving does) goes back to the newest place he stood free (never one under a ceiling lower than him), `puppet: stuck at
  ... back to ... (stall #N)`, "Snake was stuck -- back where he stood free"; with no such place the corner says "Snake is
  stuck -- hold View+Start: the Tarnished can walk out, then View+Start again". Pushing into one wall is no stall. Tested
  offline. Live 2026-10-14 only as a regression: 2 min of the real run recording looping from the Stormveil grace into the
  inside corner of a wall, a pillar and a fire basket: 0 ceiling refusals, 0 stalls, 127 wall contacts; the old boxed-in
  unstick (7 of 8 directions there) sent him 0.7 m back 55 times while the recording kept running into the corner. In the Roundtable Hold (fast travel from Stormveil's
  grace, fake MGSV standing, walked by the stick toward a spot where a ray scan of the hall found 1.62 -> 1.38 -> 0.99 ->
  0.63 m of room over the floor along one line): `puppet: a ceiling ... -- he stays out from under it`, 128 refused frames,
  Snake held 0.2 m short of it at a pedestal whose top overhangs the hall floor. The ahead probe also stops him at such
  overhangs and at table edges (his body would be inside them; the knee ray passes under a table top). The basement stairs
  of the player's report: found 2026-10-14 by a floor scan of the whole Hold (a ray down every 0.5 m: a staircase from the
  -6.6 m floor east of the hall down to a storage basement at -16.6 m, the space under it open), live result under "Low
  ceilings" above. Reaching them for a test: `puppet off`, `call setpos 51 -16.4 -1` (the Hold's arrival origin), `puppet auto`.
  The stall unstick fired live there 2 + 9 times while the test script pushed into the corners under the stairs (stalls #1-#9),
  each time back to a free place within 1.5 m.
- **Fall watchdog** (`[Puppet] fall_watch_seconds` 4, `fall_watch_drop` 60 m; `kinematic::FallWatch`, both motions): with
  NoDead set, a chasm whose death is Elden Ring's kill volume rather than a landing can't kill the puppet, and kinematic
  gravity would let Snake fall for ever (`fall_lethal_speed` only fires on a landing). A fall with no landing that lasts
  `fall_watch_seconds` or goes `fall_watch_drop` metres down is his death: a lethal `kHEvHurt` (fall) and `ChrIns::Kill`,
  as a lethal landing (`puppet: falling 3.6 s, 60 m with no landing ... -> Snake dies of the fall`). Origin shifts and
  re-anchors restart it. With root motion "falling" is the proxy going down faster than 3 m/s *with no ground within 2 m
  under it* (3 m riding; review 2026-10-13): a downhill gallop or a lift going down is no fall, and the flat at its bottom no
  landing. Tested offline (`tests/test_host_chord.inc` `TestFallWatch`: off a ledge into a chasm with no
  ground at all it fires once at 60 m, ~3.5 s; the 5 m pit lands and never fires it). VERIFIED live 2026-10-13 (no chasm
  on the way to Margit: the puppet set 150 m above his arena with `call setpos`): `puppet: falling 3.5 s, 61 m with no
  landing ... -> Snake dies of the fall (fall #1)` -> `killed the ER player`, YOU DIED, respawn at the grace 12.7 s later
  (again with the real MGSV, 150 m up from the Castleward Tunnel grace: `falling 3.5 s, 60 m with no landing` -> `guest:
  Snake died` -> respawn 13.0 s later, `kHEvRested 1`, life and ammo full)
  with `kHEvRested 1`; two of Elden Ring's origin shifts during the fall (-144 m, +32 m in y) moved the watch with it.
- **Jumps** (protocol v14 `kGuestJumping`; `[Puppet] jump`, `jump_apex` 1.2, `jump_min_room` 0.15, `jump_air_control` 3,
  `jump_air_step` 0.3, `jump_camera_hold`; `kinematic::KinematicJump`, kinematic motion only; live pass A 2026-10-14: a flat
  jump rises 1.20 m and lands after ~1.0 s, running jumps onto 1.1-1.3 m rock steps and a plank walkway at Stormveil's
  cliffside, refused under a walkway's edge and crouched; LB+R3 and LT+R3 stay MGSV's -- docs/FEATURES-1.md). The player:
  "I'm having trouble getting into Stormveil castle without that". MGSV has no jump: the guest's `[Jump]` button (R3) lifts
  Snake in MGSV and sets the flag until he lands there (docs/GUEST.md §6). On the flag's **rising edge** the puppet jumps
  through Elden Ring's world as the Tarnished would, if he stands (stance 0, not riding) on the ground:
  - the **room over his head** first: rays up from just above a step at his axis and 0.7 radius around it (seams bridged);
    less than `jump_min_room` over his height (`JumpBodyHeight`: the stance's, or the head ray + 0.2 m without `ceiling`) and
    there is no jump (`puppet: no jump -- a ceiling right over his head`); under a lower ceiling the rise is cut to what it leaves;
  - up at sqrt(2 g apex) (a step's mean speed: the apex is the one asked for); the puppet's ground velocity (eased over a few
    frames) carries on in the air, steered toward Snake's own motion by at most `jump_air_control` m/s² (Elden Ring-like: a
    little; no motion from MGSV in the air never brakes it);
  - in the air (`AirStep`, one air for the jump and the dive's hop since the merge) the knee ray drops to `jump_air_step`
    above the feet, a ledge's top up to that far above them is landed on (rising, only one above where the feet end the frame:
    the capsule catching an edge -- review 2026-10-31: "above the take-off" let ground the feet were rising past count, and a
    jump up a 12-degree slope at 60 fps, a 6-degree one at 30, or across a 2 cm curb ended 2 cm up; `TestJumpUphill`, live
    1.21-1.22 m up 14 and 24 degree slopes): a 1.0 m ledge is reached from a run or standing at its foot, a 1.7 m one never; his head stops under a
    ceiling (`headBump`: rays up from half his height, seams bridged) and a horizontal move that would put something lower
    than his head over him is refused (`BodyHeadroom`, from each probe's own ground: a table top the wall rays pass under
    and over, a beam);
  - the **landing** is the usual one, on the whole body's ground (`LandingGround`: planks and seams under his axis hold him;
    on side probes alone only where the ground snap keeps him), but a jump's landing counts as a fall only from the height it
    left from (a flat jump: 0 m/s, never fall damage; off a 5 m cliff: 9.9 m/s, as walking off it; not 10.9 from the apex);
    an origin shift in the air moves that height with the world (`KinematicShift`);
  - the odometer's height is ignored while he is in the air (MGSV's own lift and fall of Snake never count), and
    `jump_camera_hold` keeps the camera's height over Snake as it was before the jump while he is in the air (MGSV's camera
    follows his lift and fall there).
  `puppet: Snake jumps -- 1.20 m up at 5.1 m/s (jump #N)` / `puppet: no jump -- <why>`; `pr er status` counts `Snake jumped N
  (M refused)` and says `jumping`. With `motion = root_motion` it logs once that jumps need kinematic. Tested in a synthetic world
  (`tests/test_host_motion.inc` `TestJump`, `TestScaffold`, `TestJumpReview`): a flat jump (1.2 m, ~1 s, no fall damage), across a 2 m gap at 6 m/s (and into it
  without the jump), onto a 1.0 m ledge (running, and standing at its foot), not onto a 1.7 m one, under a slab 0.5 m over his
  head (the rise stops there, never on top of it), no jump under a 1.5 m ceiling (and a crouched-height body never through it),
  off a 5 m cliff (also with an 8 m origin shift in the air), air control limited to 3 m/s². The edge is remembered on every frame
  the guest's state was read, ready or not (`JumpEdge`): a not-ready frame mid-jump (a hitch, a stale Lua update) while the
  guest's flag is still up never makes the same press a second jump.
- `[Puppet] hit_hook = true` (re-backlog E2, `er/HitHook.cpp`): CSPlayerDamageModule vfunc 7
  `ReceiveDamage` (`0x44D050`) on `[ChrIns+0x190]+0x98`. While the puppet is active the original is
  skipped (no HP loss, no stagger) and the hit (amount `+0x228`, direction `-(+0x1C0)`, attacker
  distance → melee/projectile) becomes `kHEvHurt`. Not skipped once Snake has died (ER must run its
  death). Poison/fall damage still arrive through the HP-delta check.

### Dive = hop (`er/DiveHop.*`, `er/Kinematic.cpp` KinematicHop, `[Puppet] hop`, protocol v13 `kGuestDiving`; roadmap 2.9; live pass A 2026-10-14)

MGSV has no jump (Snake's own is R3: "Jumps" above; the hop is the dive's). While the guest says Snake dives (`kGuestDiving`,
docs/GUEST.md 18.17), the kinematic puppet hops: a parabola `hop_height` (0.6 m) high and `hop_length` (2.5 m) long on level ground, so a gap up to ~2 m and a
ledge up to ~`hop_height + hop_step` (~0.75 m; step_up alone walks onto 0.5 m) can be crossed. **On by default since live
pass A** (2026-10-14, docs/FEATURES-1.md: three running dives `hop #1..3: 0.60 m up ... at 3.57 m/s`, one up a slope, landed
without damage; X in a jump never arms a dive); `pr er "puppet hop off"` switches it off live, `hop = false` for good.

- **When** (`DiveTrigger`, pure): the flag's rising edge arms it; the dive's first move (>= 0.5 m/s of odometer step in Elden
  Ring) gives the hop its direction and starts it. No move within `hop_wait_ms` (300): no hop (`MGSV dived but Snake didn't
  move`). One hop per dive. Not while riding, not with `motion = root_motion`.
- **The hop** (`KinematicHop`, pure): from the ground; vy = sqrt(2 g rise), a fixed horizontal speed (length / the full hop's
  flight time) along the dive instead of the odometer while in the air (MGSV's own dive moves ~5.7 m: what is left after the
  landing moves him on as usual).
- **Off a rim** (`HopParams::graceS` 0.2 s / `graceDrop` 0.3 m, review of 2.9): the flag reaches the host a few frames after
  X (the guest's sign, the shared memory) while MGSV's dive already moves him at ~5 m/s, so at a ditch his centre is often
  past the rim when the hop comes. Off the ground less than 0.2 s and at most 0.3 m below it, he still hops, from that
  ground's height (`hop #N (off a rim 0.07 s ago, 0.03 m below it)`); later it is an ordinary fall. If the sign comes only
  late (CRAWL at the dive's end) no grace covers it: the guest's sign latency in the log says which.
- **Never through a ceiling:** at the take-off the rise is cut to the room above his head (his **standing** height
  `height_stand` + 0.05 m, whatever the stance the confirming frame reports: the guest arms a dive only on X while STAND, but
  the sign may already be CRAWL / prone, and a prone 0.5 m head would let a full hop take him under a 1.9 m beam; with
  `ceiling = false` the head ray's height); less than `hop_min_rise` 0.15 m: no hop, `dive -- no hop`); rising, rays up from
  half his height stop him where his head meets something (he falls from there); a move in the air that would put anything
  lower than his head above him (an overhang, a balcony ahead) is refused that frame (he goes on once he has come down). That
  take-off height is kept for the whole hop. In the air the hop is a jump's (`AirStep`, merge with the jump): no hop in a jump
  (`dive -- no hop (in a jump)`), no jump in a hop.
- **Never up a wall:** in the air the low wall ray sits `hop_step` (0.15 m) + 0.05 above the feet instead of step_up + 0.05,
  so a ledge is a wall until the feet are nearly over it; a ledge top within `hop_step` above the feet is landed on (while
  rising only one above the take-off ground). Apex 0.6 m: a 1 m wall stops him every time, he lands back at its foot.
- **Landing:** a jump's, on the whole body's ground (`LandingGround`: "Collision filters and the ground under Snake" below --
  planks with gaps hold a hop's landing as a fall's): `r.landed` -> `LandingDamage`, counted as a fall only below the
  take-off's height (a level hop: 0 m/s; before the merge ~3.4 m/s, no damage either); a hop off a cliff becomes an ordinary
  fall after 1.25 flight times (the fall watchdog and fall damage as for any fall).
- Logs: `puppet: dive -> hop #N: 0.60 m up toward (x, z) at 3.57 m/s from (...)` (`(lower: a ceiling)` when cut),
  `puppet: dive -- no hop (...)`; `pr er status` counts `hops N (refused N, no move N)` (`[hop off]` when off).
- Tests (`tests/test_host_hop.inc`, synthetic world): level hop 2.5 m / 0.6 m at 60 and 240 fps; a 1 m gap crossed (walking in
  he falls; both at 1.5 m/s, so the far rim is never caught as a step: rise, apex and a landing past z 2.5 checked); the hop
  0, 3 and 6 frames after walking off the gap's rim at 5 m/s still crosses, 0.27 s later it is refused; a 0.6 m ledge climbed from 0.3-1.0 m away (walking, stopped); a 0.5 m ledge with step_up 0.3; a 1 m wall never
  crossed from four distances, five hops in a row; a 2.1 m ceiling over a standing Snake caps the head at 2.1 m, 1.9 m gives
  no hop -- also when the confirming frame reports prone or crouched (the standing take-off height), and under 2.1 m told
  prone the head still stays out (crouched at the take-off, room again); an overhang 2.0 m up ahead never takes his head
  (also told prone); no hop under a low ceiling over a gap (he
  falls in as walking); no second hop in the air. Merges: at the foot of stairs the full hop (head room from each probe's
  own ground); along a gap between a scaffold's planks it lands on them (one ray: through, 3 m down); no hop in a jump, no
  jump in a hop; a level hop lands at 0 m/s; walking into the 1 m gap at 4 m/s falls in (no step up its far face in mid-air).
### Collision filters and the ground under Snake (`er/Kinematic.cpp` BodyGround, `[Puppet] ground_filter`, `wall_filter`, `wall_filter_also`, `ground_probes`)

Player report 2026-10-14 ~19:54: the Tarnished climbed the wooden scaffolding on Stormveil's side; switched back to Snake at
the top (`puppet ON ... ER feet (-3.15, 3.34, 0.69)`), Snake fell straight through it and died (`landed at 12.7 m/s -> fall
damage`, ~8 m). Two causes, both fixed (unit-tested; verified live 2026-10-14 on that scaffold, below):

**1. One ray under his centre.** Wooden floors are planks with gaps; the ground snap, the landing check and the puppet's start
each cast a single ray under the body's centre, and a gap reads "no ground". Now `BodyGround` casts five: the centre and four
points at 0.8 x the body radius (inside the body, clear of a wall it stands against; the cross turned 30 degrees, so planks
laid along one direction see five different offsets across their gaps, not three). Where he stands:
- **the centre found ground**: that ground, as the single ray had it (a slope, a step's edge). Standing on the highest held
  probe instead (the first version) floated him over every slope -- the uphill probes are higher: 6/13/18/25 cm at 10/20/30/40
  degrees (review 2026-10-14), and the head room lost as much (a sloped tunnel as tall as Snake refused him). Unless three or
  more side probes stand on ground above the centre's: a slope or a step's edge lifts two of four at most (they are 90 degrees
  apart), so that is a gap between planks under the centre, and he stands on the plane those probes agree on (opposite probes'
  mean: on any plane exactly its height under the axis);
- **the centre found nothing** (a gap down to a drop, a rim): the side probes' plane (an opposite pair both found ground), else
  the highest probe that a second probe holds (another within 0.1 m below it). One probe on a step's edge or a shaft's rim is
  no support: he still falls off edges (two probes hold a walking body until its centre is 0.14-0.20 m past the edge, by
  heading) and into holes wider than the probes (~0.5 m).

Used by the ground snap (step up / snap down), the landing check (`LandingGround`, for a fall, the dive's hop and the jump
alike: the frame's whole move, from 0.1 m above its higher end -- a hop's `hop_step` + 0.05, a jump's `jump_air_step` + 0.05
-- to 0.05 m below its lower end: consecutive frames overlap, so a thin platform is never stepped over at any speed; a plain
fall going up lands on nothing; and on side probes
alone he lands only where the ground snap would keep him: at a ledge's rim with the floor within `snap_down` below his centre
he would step down at once, so he falls on past it -- merge with the dive's hop: a hop onto a 0.6 m ledge landed on its rim and
snapped back down) and `SupportedStart`. **The start stands on firmer ground than a walk keeps** (`StartGround`): the centre's own
ground, or four of the five probes at the body's level (planks with gaps under the centre). Two probes on a rim with the
centre over a drop hold a walking body, but a start there (the first version) left Snake 4 cm from losing the second probe:
the Fort Haight shaft rim again (review 2026-10-14). Such a start is moved, as before, to the nearest whole-body place (centre
and four points at the full radius on ground), or failing that at a distance to one where all five probes stand (four is not
enough: the cross is fixed in world axes, and with an arm square to the edge four probes stand with the centre a centimetre
inside it; five keep it 0.2 m or more in). `[Puppet] ground_probes = false` is the old single ray. Tested in the synthetic
world (`tests/test_host_motion.inc` `TestGround`: a 3 m scaffold of 22 cm planks with 6 cm gaps and a 0.8 m hole, a 2 cm
platform, a boardwalk 0.3 m above the ground, 25-degree ramps: solid, of planks with gaps, and a tunnel 1.9 m tall):
standing over a gap (the player's case; the old ray fell 3 m), walking across, along and diagonally over the planks with no
dip, the hole and the edge still drop him, a 5 m fall onto the planks lands at ~9.9 m/s (the old ray: through to the ground),
a 30 m fall (~24 m/s) lands on the 2 cm platform, the start over a gap stays, starts 0.1 and 0.2 m past the edge move onto
the planks; on planes of 10-40 degrees at 24 headings (radius 0.35 and 0.45) the ground is the one under the axis within
5 mm, with nothing or deeper ground under the centre too; up and down the ramps (planks too) the feet stay within 2 cm of the
slope and the 1.9 m tunnel never refuses him. `tests/test_host_chord.inc` `TestSupportedStart`: starts 1-29 cm past a pit's
edge, the world turned 0-75 degrees, all move 0.19 m or more in.

**Live pass A (2026-10-14):** on the cliffside scaffold (the zig-zag wooden stairs above Stormveil Cliffside's grace) a
Tarnished-mode switch on the planks started with `below the start: -0.05 m (ground_filter), -1.39 m (ray_filter 0x2000058)`;
Snake stood 10 s, walked the landings and up and down the flights with no `left the ground`, 0 ceilings, 0 unsticks; the
only walk-off lines were real edges. The Roundtable Hold's basement stairs climb as before (docs/FEATURES-1.md).

**2. The ray filter.** `cs_phys_world_cast_ray(world, filter, from, extent, hit, ignore)` (0xC71E00) builds an hknp ray query:
`filter` is the query's collision filter info, run through Elden Ring's `CS::CSCollisionFilter` (vtable 0x2B94DF0; the
instance at `CSHavokMan+0xC0`), then the hits go to a `CS::CSClosestHitCollectorForSnipe` (0xC5A9F0; its addHit 0xC5ABF0
also skips one object tied to the ignored character, `[[chr+0x190]+0xE8]`, probably his mount). RE'd (Ghidra +
`tools/disasm.py`, 2026-10-14):
- filter info bits: **0-6 = layer** (0-127), 7-8 = a group (two bodies in the same non-zero group never collide), 25-29 = a
  sub-id (the query's character's own bodies with the same non-zero sub-id are skipped: 0x2000000 = sub-id 1), bit 30 = "only
  its own character's". The check (`isCollisionEnabled`, 0xC63220 query vs body, 0xC63320 body vs body) reads a 128 x 128
  bit matrix at `filter+0x20` (`row[queryLayer] >> bodyLayer`).
- The matrix is built in the constructor (0xC5EE70 -> 0xC5F1F0, ~750 calls of enable/disable/copy-row helpers 0xC624A0 /
  0xC62410 / 0xC5F160 and group helpers 0xC62DB0 / 0xC62A90 / 0xC62C10 / 0xC62800 / 0xC62530). Emulated from the
  disassembly, the rows that matter:
  - **characters**: layers 2-10 (pairs 3/4, 5/6, 7/8, 9/10 collide with each other; 0xE collides with every character
    layer and nothing else). Their world: layers 0x37-0x4A come as 4 groups x 5 variants (0x37 + 5g + v); characters
    collide with variants 1-3 (0x38/0x3D/0x42/0x47, 0x39/0x3E/0x43/0x48, 0x3A/0x3F/0x44/0x49) plus 0x4B, 0x31, 0x35, 0x52,
    0x54. Layer 3 (and 5, 7, 9) also collides with group 2 (0x42-0x44), the even ones (4, 6, 8, 10) with 0x4F-0x51 instead:
    probably player-only and enemy-only walls (UNVERIFIED which is which).
  - **0x58, the old `ray_filter`** (the game's only caller of cast_ray, 0x3FB990, the main player's aim/sight ray):
    variants 0, 1, 3, 4 and 0x33, 0x4D, 0x52-0x54, plus 0x1D-0x20, 0x23, 0x24, 0x27, 0x28 (probably characters' hit
    capsules). It misses **variant 2 (0x39, 0x3E, 0x43, 0x48), 0x4B, 0x31 and 0x35** -- collision only characters meet
    (floors, ramps and walls for walking) -- and hits variant 0 (0x37, 0x3C, 0x41, 0x46), which characters pass through.
  - **0x16** (and 0x15, 0x17-0x1B: one per character layer 2, 3, 4, 9, 10, 0xB, 0xC) = a character layer's row with every
    character layer 2-0x2D removed (0xC5F160 then 0xC62530): **exactly what the character's capsule collides with, no
    characters**. 0x16 is layer 3's: 0x31 0x35 0x38 0x39 0x3A 0x3D 0x3E 0x3F 0x42 0x43 0x44 0x47 0x48 0x49 0x4B 0x52 0x54 (and
    0x0 / 0x69, which collide with everything).
- The collector (0xC5A8E0, shared with the camera's) drops hits whose material class (`0xC75570`, hundreds digit) is 2 or 5.
  Not reachable through cast_ray's filter.
- The query's byte at +0x24 (0xFB) is a **broad-phase layer mask**: the world's `hknpWideBroadPhase` (`[hknpWorld+0x4D8]`, its
  cast vfunc 0x80 = 0x18A04D0) visits a layer only when that byte ANDs its mask byte (`layer+0x19D0`; live: 6 layers, masks
  0x02 0x03 0x02 0x01 0x01 0x04) and the collision filter's vfunc 7 lets the layer through. 0xFB leaves out the layer whose mask
  is 0x04.
- **Snake's own ray query** (`game::CastRayKeep` / `CastRayAll`, 2026-10-14 live pass): cast_ray's query rebuilt byte for byte
  (address table `cast_ray query build` 0xC71E8D, `cast_ray query calls` 0xC71F56: codec `[hknpWorld+0xB00]`, filter
  `[CSHavokMan+0xC0]`, material 0xFFFF, filter info, the ignored chr, 2 at +0x20, the layer mask at +0x24, the ray at +0x30)
  with a snipe collector (ctor 0xC5A9F0, dtor 0xC5AAB0) whose vtable is a copy with addHit replaced: a hit of class 2/5 in
  `[Puppet] ground_keep` (bit per class) goes to `hknpClosestHitCollector::addHit` (0x1886510) instead of being dropped, any
  other to the game's own (0xC5ABF0); `ground_layers` is the +0x24 byte. All of it byte-verified (80/80) or the body rays use
  cast_ray (logged once). **Off by default** (`ground_keep = 0`, `ground_layers = 0xFB` = cast_ray itself): at Stormveil's
  cliffside scaffold neither mattered -- every floor there is layer 0x39 (0x38 a few), materials 1/3/4/101/104 (a 1935-ray
  survey of x -26..-5, z 10..32 with all layers: no class 2 or 5, nothing in the 0x04 layer), and the planks 0x58 misses
  are found by 0x16. Diagnostics: `call rayall x y z dx dy dz [filter] [layers]` lists every hit (fraction, point, normal,
  body, its layer and filter info, hknp material, shape key, Elden Ring material id, "dropped by the game's collector");
  `call raykeep ... [filter] [keep] [layers]` is the closest hit as the body rays would get it; `puppet set ground_keep 0x24`
  / `ground_layers 0xff` switch the body rays live. The Tarnished's own ground: `[[player+0x190]+0x68]+0x250` (i32, the
  physics module's hit material, 4 = wood, 1 rock) and `+0x230` (the contact normal).

So: **`[Puppet] ground_filter = 0x2000016`** (layer 0x16, sub-id 1) for Snake's vertical rays (ground, landing, step, head
room, the start; also root motion's "ground under the proxy") -- everything the Tarnished stands on, no characters --, and
his walls through **both** `wall_filter = 0` (= `ray_filter` 0x2000058: enemies' hit capsules stop him) **and
`wall_filter_also = 0x2000016`** (the characters' own walls), the nearer hit kept (`er/Kinematic.h` `BodyRays`; 0 = only
wall_filter). The first version had walls on 0x58 alone: a scaffold whose floor only 0x16 sees most likely has its railings
and boarding in the same characters-only layers, and Snake, now standing on the floor, would have walked through them off
the edge (review 2026-10-14; unit-tested: a railing only in those layers stops him a radius short, walls on 0x58 alone walk
him off 3 m). Twice the wall rays' cost. UNVERIFIED live, and so is which character layer is the player's (3 or 4: layer 3
also collides with 0x42-0x44, maybe player-only or enemy-only walls): if Snake stops at an invisible wall the Tarnished walks
through, `wall_filter_also = 0` (and report where). `ray_filter` stays 0x2000058 for sight, bullets, the camera, mines and
buddies (sight and bullets shouldn't meet character-only walls). `BodyRay` (`er/Puppet.cpp`) picks the filters per ray:
mostly up/down -> ground_filter, else wall_filter and wall_filter_also. All but ray_filter need the address
table's `collision layer 0x16 (CSCollisionFilter)` (the bytes at 0xC6004E that build row 0x16 from row 3: `mov edx,0x16; lea
r8d,[rdx-0x13]; call 0xC5F160; mov edx,0x16; call 0xC62530`, verified on the exe); without it Snake's body uses `ray_filter`
(logged once). Diagnostics: at the puppet's start `puppet: Snake's body rays: ground_filter ..., wall_filter ... + <also>,
below the start: <dy>
(ground_filter), <dy> (ray_filter)`; each time he walks off ground (not a jump, not a hop) `puppet: Snake left the ground at
(...) [N frames after the start] -- below him: ... (ground_filter ...), ... (ray_filter ...); his five ground rays (centre
first) down to 3 m: ...` with what both filters see below and each of `BodyGround`'s rays (the hit's depth below his feet,
`-` none: nothing under any ray where the Tarnished stood means the filter doesn't see that floor), the first 20, any in a
start's first 2 s, then every 50th. Try a filter by hand: `call ray x y z dx dy dz [filter]`.

**In the air** (merge with the jump, 2026-10-14): a fall, a hop and a jump are one air (`Step`'s airborne pass): gravity at
the frame's mean speed (a jump's apex is the one asked for), the landing on `LandingGround`, and the knee ray low: in a plain
fall 0.1 m above the feet (`kFallStep`), in a hop `hop_step`, in a jump `jump_air_step` -- a ledge is a wall until the feet are
nearly at its top. Before, a falling body kept step_up's knee ray (0.55 m): moving while falling past a ledge's face it walked
into the face below its top and "landed" inside it, popping up (with the probes' rim hold, a 6 m/s walk crossed a 2 m gap
0.4 m down into its far side; `TestJump`, `TestHop` 4 m/s into a 1 m gap: both fall in now).

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
- **Walking into breakables** (kinematic, the body-bump breaker): the player confirmed it in real play (2026-10-12
  evening): breakables break when Snake walks into them, and when he shoots them.

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

### Projectile meshes and muzzle flashes (roadmap 5.4: `er/Projectiles*`, `er/ProjectileLogic*`, `[Render] projectile_meshes`, `[Combat] muzzle_flash`)

Both are **on by default since 2026-10-31** (live switches: `pr er "throw meshes on|off"`, `pr er "boom set muzzle_flash on|off"`).
Live pass A (2026-10-14) had no grenade draw tagged by the guest and a 1-3 m fireball for a flash. 2026-10-31, Elden Ring 1.17.2:
- **Projectile meshes** with the MGSV agent's new real-MGSV recordings (guest tagging on; `recordings/README.txt`):
  `throw_grenade.prrec` -> `projectiles: throw equip 354 drawn with MGSV's mesh from now on: 1 part(s), first mesh
  b1332cea2c489926` on the first release, `measures 0.14 m across`, 747 copies drawn with it over two loops; frames: MGSV's dark
  grenade with its red band flying along the arc where the olive ball was. `fire_rocket.prrec` -> `round bullet 46 drawn with
  MGSV's mesh: 2 part(s) ... 0.53 m across` (the rocket hit the church wall 1 m out: no flight frame). Also fake MGSV's own tagged
  grenade stand-in (`!projmesh on`, `--model humanoid`: a 9 cm grenade drawn as `kOwnerProjectile` while it flies): learned
  (`0.20 m across`) and drawn instead of the ball at the same spot (`throw meshes off` shows the ball there). A guest that tags
  nothing leaves the ball, so the switch is on.
- **Muzzle flashes**: the look is now the Fire Pot's bullet with the Spark Aromatic's burst of sparks (`explosion_muzzle =
  10030001 300522`, ~0.4 m) for `muzzle_life` 0.1 s (at 0.06 s it never showed). Tried side by side in front of the Tarnished
  (`boom look muzzle <bullet> <sfx>`, `boom set muzzle_life 0.3`, `boom muzzle`): 300003 / 5300003 (deflection sparks) show
  nothing, 300002 / 5300002 a throwing dagger / a ballista bolt, 300172 a 2-3 m lightning burst, 300523 a 1 m fireball, 300122
  the Fire Pot's fireball (lights the scene orange). Live with fake MGSV's `!fire rifle`: one small burst at the muzzle point per
  shot. Not yet with the real MGSV's muzzle points.
- **With both real games** (2026-11-01, the Church of Elleh's meadow at night): a **sleep grenade** thrown (LT, then an RT tap) ->
  `throw 1: sleep (equip 378 ...) released`, `projectiles: throw equip 378 drawn with MGSV's mesh from now on: 1 part(s), first mesh
  2142aaa5a2f2dabc ... 0.22 m across` 226 ms later (the guest: `first thrown draw tagged ... 8 frames after the release`); a frame
  0.5 s after the release shows MGSV's blue sleep-grenade canister in flight over the grass, the blast 4.98 s later 15.8 m out.
  **Rockets** (FB MR R-LAUNCHER, three, into the open): `round bullet 46 drawn with MGSV's mesh from now on: 2 part(s), first mesh
  558639bdf551fb9f ... 0.53 m across`, hits 105-129 m out (MGSV) 2.4 s after the shot; frames every 50-170 ms from the release to
  2.7 s: the rocket flies straight away from the camera, so only its bright exhaust shows (a large glow at the launcher for the
  first ~0.3 s, then a shrinking white dot down range) and the orange blast at the end -- its body is never seen side on. The guest
  also tagged four or five rigid MGSV meshes (0 bones, 58-475 vertices) near a rocket's path; they are not the learned template,
  so they are left out (`60 unclaimed`), not drawn. **Muzzle flashes** with the real MGSV's rifle: the frame of a burst shows
  orange sparks in front of the SVG-76 (`muzzle flashes on (17 shown, 0 over muzzle_per_second)`).

**Projectile meshes.** The flown copies of Snake's grenades (`er/Throw.cpp`) and rounds (`er/Rounds.cpp`) were a small olive
ball and a dark ball with a flame. The guest now publishes MGSV's draws of a projectile it just released with owner
`kOwnerProjectile` (protocol v13; `mgsv/ProjectileTag.*`, docs/GUEST.md 15.5). The host:
1. notes each release (`Throw::OnThrow`, `Rounds::Launch` -> `Projectiles::NoteRelease`) with its direction in MGSV world
   (`Anchor::DirToMgsv`), keyed by what was released: the throw's equip id, the round's bullet id;
2. in `RenderScene::Capture`, right after the draw list is copied -- also when MGSV sent none that frame
   (`Projectiles::Apply`): the tagged draws of a list within 1.5 s of an unsettled release become a *candidate* -- each draw's
   record (mesh, textures, material) and its matrices turned into the projectile's own frame (z along its flight, y up as far
   as it goes, origin = the first draw's; `projectile::Learn`; more than 6 parts or origins spread over 1.5 m: not one). The
   candidate is taken only when a later list, 3-20 MGSV frames on, shows the same meshes moved at least 2.5 m/s within 60
   degrees of the release direction (`projectile::Confirms`): Snake's gear and a prop down range stay still in the list's
   space (his root), so a mis-tag is never learned from one list. A release gives up after 8 lists without that ("never
   flew", warned). The first confirmed template of a key stays; another one replaces it only when 2 releases in a row agree
   on other meshes (warned). When its meshes are on the GPU its real size is measured once from their boxes
   (`projectile::Across`, logged "X m across"): over 0.5 m for a throw or 1.2 m for a round it is refused (warned; the ball
   stays, the next confirmed learn replaces it). `kOwnerProjectile` draws are removed from the list (MGSV's grenade flies over
   MGSV's airport, not Elden Ring). With the switch **off** nothing is learned or placed and tagged draws go back to where the
   guest put them before v13: Snake's within 2.5 m of his root (`[Render] draw_player` on), else out
   (`projectile::HandBack`);
3. each frame, every flown copy whose template's meshes are on the GPU (`RenderAssets::FindMesh`) is drawn with it: the
   template placed at the copy's Elden Ring point, turned along its velocity (a thrown one tumbles about its flight at
   `projectile_spin` rad/s while it moves; one that slows keeps the heading and spin it had, `projectile::Pose` per copy),
   brought into the list's space through the inverse of
   the snapshot's anchor matrix (rotation about Y with the optional X mirror: its transpose), so the renderer's own anchor
   matrix puts it back exactly where the copy is. It is an ordinary rigid draw from then on (lit, shadowed, near-faded as an
   "other" owner, velocity from the previous snapshot's same draw). `Projectiles::Drawn(source, copy, frame)` tells
   Throw/Rounds to leave out the ball only for a copy really placed in the last scene (a rocket keeps its flame, exhaust and
   smoke trail); tagged draws pass `[Render] draw_other = false` so they can still be learned.
No template yet (the first throw of an item ever -- or in each session with `projectile_store` off --, a guest from before v13 or with `[Capture] ProjectileMeshes` off
-- its default --, a mesh forgotten after the guest re-attached, a template refused as too big), or the switch off: the ball
as before. `throw` on the host console prints the templates (`pr er throw`).

**Kept across sessions** (0.7.0; `[Render] projectile_store`, true; `PhantomRingHost.projectiles` next to the ini and the DLL). A
template learned or measured is written to disk (via a `.tmp` and a rename): per key (throw equip / round bullet id) its size, its
parts as the guest's draw records -- its content hashes of mesh, textures and material, no MGSV asset -- with each part's matrix,
and whether it was refused (magic `PRPJ`, version 1; anything else in the file: ignored and logged). A later session reads it
once, the first time it needs a template, and draws a key as soon as MGSV has sent those meshes (the hashes are the guest's own
keys, so MGSV's same meshes find the same template): `projectiles: throw equip 354 known from an earlier session: 1 part(s), first
mesh b1332cea2c489926, 0.18 m across`. Loaded templates follow the rules of learned ones (two releases in a row agreeing on other
meshes replace one). Live 2026-11-03 (fake MGSV's tagged grenade, then a new Elden Ring session): learning took 0.256 s after the
first release in the first session; in the next `projectiles: throw equip 354 drawn with the mesh kept from an earlier session
(0.02 s after its release: no learning needed)`. `projectile_store = false` learns every session as before (the offline tests
set it, so a store in build/ can't change their results). Review 0.7.0: a store with a part of no matrix (it reached the renderer
as a draw reading past the matrices), a non-finite number anywhere, an extent over `kMaxExtent` or a size over the key's limit is
ignored whole ("unreadable ... learned again"), and the renderer drops any draw whose matrices aren't all in the snapshot. Live with
both games (0.7.0 integration): `projectiles: throw equip 378 known from an earlier session` at the load, then the session's first
sleep grenade `throw 1: sleep (equip 378 ...) released` -> `drawn with the mesh kept from an earlier session (0.02 s after its
release ...)`; a frame 0.25 s after the release shows MGSV's blue canister inside the throw ring.

**Muzzle flashes.** Every `kGEvFire` / `kGEvBuddyFire` that is not D-Walker's flamethrower calls `Explosion::Muzzle`: the
eighth explosion look, `kMuzzle` (`explosion_muzzle = 10030001`, the Fire Pot's fireball) written into the eighth carrier
(`10302005`, Miranda's Prayer's cut chain, unreferenced: `test_host_combat` scans every param) as an effect-only copy
(`MakeCarrier` without its children: no sparks, no ground flames) that does not move, lives `muzzle_life` (0.06 s) and takes
its effect with it when it dies (`MakeFlash`: `sfxDeleteByLifeDead` 1), so the fireball is cut after its first flash.
Spawned `muzzle_ahead` (0.1 m) in front of MGSV's muzzle point along the shot, at most `muzzle_per_second` (10; automatic
fire shows a flash every few rounds), counted in our live-bullet cap like every look; none for a suppressed shot (protocol v16
`kFireSuppressed`: the tranquilizer pistol, a suppressed rifle) unless `muzzle_suppressed` (0.7.0; before, `muzzle_tranq` for
tranquilizer rounds, still read as its fallback); a worn-out suppressor's shots flash. `boom muzzle` on the host console shows one in front of the Tarnished.
If the fireball reads too big even cut short, try another look live: `boom look muzzle <bullet id> [sfx]`.

### Noise: Snake's shots heard by Elden Ring's enemies (`er/Noise*`, `er/NoiseLogic*`, `[Noise]`; protocol v16 `kFireSuppressed` / `kGuestSuppressorOn`; 0.7.0, live 2026-11-03)

MGSV's shots are no Elden Ring attacks, so they made no sound its AI could hear: an unsuppressed rifle beside a camp alerted
nobody. Elden Ring's enemies hear *AI sounds*: every attack of the Tarnished's makes one (AtkParam appear / hit AI sound ids:
AiSoundParam 2100, 4 m, as a swing starts; 2010, 12 m, where a weapon hits), and so do his footsteps, pots, breaking crates and
the enemies' own animation events.
- **The sound system** (Ghidra on 2.7.2.0; `A::WorldAiManager`, `AiSoundRequest`, `AiSoundEmit`, `AiSoundAdd` byte-verified on
  2.7.1.0 and 2.7.2.0 like every address -- one failing leaves `sound` mode unable to emit, logged once with the reason):
  CSWorldAiManager +0x7628 holds a vector of 0x90-byte sounds behind a lock (+0x20). `0x140348760` builds a request from a
  position, an owner record (the hidden Tarnished's handle: he stands where Snake does, so the sound is the Tarnished's -- hostile
  to the enemies, ignored by allies) and an attack's AI sound (`sound_row`, 2010); `0x14034A1B0` adds it. The built request's
  handle (+0x18) and row (+0x48) are checked before the add; the radius is the row's x the request's scale (+0x4C), so
  `noise::ScaleFor` gives each weapon class its own. Each AI's listener checks the sounds near it every update (within the radius
  plus its ear distance, a hostile owner, the rank) and takes the loudest as its sound target -- the turn, the walk over to look,
  the search -- through its own NpcThinkParam sound behaviour.
- **Each shot** (`Noise::OnShot` from `Combat::OnShot`, every `kGEvFire` / `kGEvBuddyFire`): a sound at the muzzle, its radius by
  the class of MGSV's bullet (`noise::ClassOfShot`: Hg* pistol 20 m, Rf* rifle and machine gun 35, *Sniper* 40, 12GA* shotgun 45,
  40 mm / ms* / vehicle guns and anything explosive launcher 55, D-Walker's flamethrower 10); an unsuppressed round that strikes
  the world makes `impact` (5 m) where it lands (a character it hits makes Elden Ring's own); at most `per_second` (8).
- **Suppressed = silent all the way** (`b & kFireSuppressed`): radius `suppressed` (0: no sound), no muzzle flash (`[Combat]
  muzzle_suppressed`, false; it was `muzzle_tranq`, still read as the fallback), and the dart's hit bullet carries no sound either:
  the Throwing Dagger AtkParam_Pc row (10170000) it is built from has appear 2100 / hit 2000 (4 m round the target); a quiet hit
  (`HitEffect.quiet`, its own `HitKey` bit 53, so never sharing a patched row with a loud one) writes both -1, a loud one the
  row's own values back (`er/Combat.cpp` SpawnHitBullet).
- **`mode = heard`** (the fallback, ini-switchable): enemies within the radius that are not fighting yet get a token hit (no damage,
  no flinch: poise 0.01, damage level 0, quiet) -- Elden Ring's aggro on the Tarnished, not a search. `off`: no noise. Since the
  0.7.0 review the tokens wait until the shot's own hit has left the patched row (they used to take it first: a dart then lost its
  sleep, a lethal round its flinch -- `Noise::Tick` runs a waiting shot's tokens once `Combat::RowBusy()` is false, a shot's waiting
  at most 1.5 s), go to at most 4 foes a shot, nearest first, and never to the shot's own target, a sleeper (`Combat::Asleep`: an
  Elden Ring hit wakes him), a foe CQC holds or one token-hit in the last 10 s.
- **The AI's state** (`noise who`, mode heard): CSAiTargetingSystemOwner's slots 14 / 15 (is_battle_state /
  is_not_in_any_search_state) are byte-verified functions in both tables since the 0.7.0 review (`A::AiOwnerIsBattle` 0x345E50,
  `A::AiOwnerNoSearch` 0x345E60, the same code and RVA in 2.7.1.0's Ghidra project and 2.7.2.0's exe; `verify_er_addresses` 89/89 on
  2.7.2.0), and ReadAi calls one only when the live vtable slot is that very function -- otherwise the state is "unknown"
  (logged once) and mode heard hears each foe once per 10 s.
- **A guest that never reports a suppressor** (review 0.7.0): kFireSuppressed only says "suppressed", never "unknown", so a guest
  whose suppressor reading failed its byte checks would turn every dart into a loud pistol shot. Until the guest reports one this
  session (`kGuestSuppressorOn` in a state or `kFireSuppressed` on a shot; a re-attach starts over), tranquilizer rounds stay quiet,
  the pre-v16 rule (`noise: the guest never reported a suppressor -- tranquilizer rounds kept quiet (pre-v16 rule)`, once).
- **"Suppressor worn out"** (corner, warn; `noise::SuppressorWatch`, `worn_note`, `worn_window_ms` 1000): kGuestSuppressorOn going
  off within 1 s of a suppressed shot and staying off 0.3 s, the weapon in hand at that shot (the GuestState's weaponId: the shot's
  own equip id is another numbering) still in hand. A weapon switch (in either order: the id first or the flag first), holstering,
  MGSV's menu, a climb, a vault, CQC, the box, a vehicle, a death, a load or a cutscene next to the drop is no wear; the watch
  starts over with each guest. (Before the review a switch could give a false note.)

Console: `noise` (mode, radii, sounds made / quiet shots / over the cap / failed / heard, suppressors worn out), `noise emit [r]
[m ahead]` (a sound in front of the Tarnished), `noise mode sound|heard|off|ini`, `noise set <key> <v>`, `noise who [r]` (each
character near: team, distance, HP, calm or BATTLE, its last AI sound and rank, search slots, "(sound target)"; the NPC's AI is
found once by RTTI: `noise: an NPC's AI is ChrIns +0x580 -> +0xc0 (its targeting owner +0xc938 names the ChrIns, its targeting
system +0xc950)`). Fake MGSV: `!suppressor on [shots] | off`, `!fire <weapon> suppressed|loud` (wears out after `shots`). Tests:
`tests/test_host_noise.inc` (classes, radii, scale, the watch, quiet hit keys), tools/phantomring/test_fakes.py (v16 flags).

**Live 2026-11-03** (Elden Ring 1.17.2 headless 4K alone, fake MGSV, Stormveil; `noise who` sampled every ~0.47 s):
- `noise emit 60` -> `noise: AI sound 60 m (console; AiSoundParam 2010 x 5.00)`: two soldiers 47.1 / 47.6 m away took it as their
  sound target 0.36 s later (`last AI sound 2010 (rank 1) ... (sound target)`) and walked 12 m towards it in 10 s, still calm.
- Snake's pistol, unsuppressed -> `noise: AI sound 20 m (Snake's pistol; AiSoundParam 2010 x 1.67)`: the four soldiers at 11.0,
  13.0, 13.4 and 17.7 m had it as their sound target 0.35 s later; npc 36600014 at 22.8 m (outside 20 m) and the sentry 30003514
  at 13.3 m did not. The same shots suppressed from the same spot: nobody's last sound changed (the status counts them as quiet shots,
  no sound made).
- **A dart in a group**: a suppressed head shot on one of three soldiers in a courtyard -- frame 4 s later: he lies asleep, the
  two beside him stand as before; 25 s of `noise who`: every one `calm ... last AI sound 0 (rank -1)`. The control from the same
  spot, an unsuppressed pistol shot: all four soldiers near had it 0.35 s later, walked over from 2.3 s, were in BATTLE 5.2 s
  after the shot and at Snake (1.7 m) 8.5 s after it.
- `!suppressor on 3` then three suppressed shots: `noise: the suppressor on Snake's weapon 291 wore out (kGuestSuppressorOn
  dropped right after a suppressed shot)`, the corner "Suppressor worn out" (frame), and the next shot loud (`AI sound 20 m`).
- Not every enemy listens: the upper rampart's 42101014 and 30100114 (27.9 / 29.5 m from a 60 m sound) and the sentry 30003514
  never registered one (their think params may have no sound behaviour); the sentry raised its alarm when the Tarnished was set
  down next to it. Not tried: the real MGSV's suppressed weapons, `mode = heard` live, D-Walker's guns.

**Live with both games 2026-11-03 evening** (0.7.0 integration: the real MGSV 1.0.15.4 and Elden Ring 1.17.2, the player's save at
Stormveil, the same courtyard group of four): `noise who` reads `calm searching` through the verified getters. The real WU S.PISTOL,
aimed through MGSV's own camera: guest `shots: equip 7 SUPPRESSED (suppressor life 30 before the shot)`, host `Snake tranquilizer
(weapon 7 bullet 10 x1, suppressed) -> npc 30000014 ... HEADSHOT ...: bullet 0 poise 0 lv 0 sleep +252 (resist 229, 2 rungs)`, `1
quiet shots, 0 AI sounds made`; 18 s of `noise who` (108 samples) after it: all four `calm ... last AI sound 0`. The control, an SVG-76
round into the ground beside them: `AI sound 35 m (Snake's rifle)` + `5 m (... strikes)`, all four had 2010 at the next sample, three
in BATTLE 1.9-2.7 s after the shot (the darted one among them: the loud round woke him). Wear-out with the real MGSV (the save's
Infinity Bandana taken off in memory, `vars.playerFaceEquipId = 0` + `!boot`, saves blocked; `!suppressor 2 3 on`): the guest's
life before each shot 3, 2, 1 ("it wears out with this one"), the host `the suppressor on Snake's weapon 7 wore out` 316 ms after
the third (the 0.3 s confirm), the corner "Suppressor worn out" and MGSV's weapon box without its gauge (frame), the fourth shot
`AI sound 20 m (Snake's pistol)`; a rest at the Rampart Tower grace: `suppressor: refill (rest; slot 2: 0 on -> 30 on ...)`, the next
shot SUPPRESSED (life 30).

### Decoys (`er/Decoys.cpp`, `er/DecoyLogic.*`, `[Decoys]`, protocol v13 `kGEvPlaced` a = `kAmmoDecoy`; roadmap 2.8; live pass B 2026-10-15)

MGSV's decoys (inflatable 397-399, active 401-403, shock 405-407 / 380-381) only fool MGSV's soldiers, and its sandbox has
none. The guest now announces them like a mine (`kGEvPlaced`, a = `kAmmoDecoy`, trigger 0; docs/GUEST.md 18.8) and the host
keeps each where Snake set it (anchor at that moment, set down on Elden Ring's ground through Snake's ground filter --
`Puppet::CastWorld`, review 2026-10-31: ray_filter passed through the character-only floors he stands on, Stormveil's scaffold --
near that height first, else down to 40 m below, else from 12 m above -- MGSV throws a decoy onto its flat apron, and live one thrown down a slope stood 5.5 m up in the
air), drawn as a soldier-shaped grey-olive
balloon of lit balls (`decoy::Figure`, `height` 1.75 m) that blows up over 0.8 s and collapses over 0.5 s when it pops; the
active decoy's speaker blinks amber, the shock decoy shows a steady blue charge (`RenderMarkers::Owner::kDecoys`).

Four times a second one `Combat::HostilesNear` scan around the standing decoys (`decoy::Notices`, pure):
- a hostile whose eyes (top of its capsule) see one within `range` 15 m (`cone` 70 deg in front, a clear ray) stops and stares
  for `stare` 6 s -- the box poster's mechanism (`er/Stealth.cpp`): ChrIns `+0x538` no-attack / no-move bits, only the bits
  we set are cleared again; `cooldown` 15 s before the same one stares again. Never a boss (`decoy::Fooled`, `Cqc::IsBossHp`;
  review 2026-10-31: a boss within 25 m of an active decoy froze 6 s of every 21 s) -- a boss still pops one it walks into --
  nor one CQC holds; the box poster's stare (`er/Stealth.cpp`) leaves bosses and sleepers be too. An **active** decoy is heard: within
  `active_range` 25 m, no cone, no ray. Asleep enemies don't notice; one within `snake_close` 4 m of Snake and nearer him
  than the decoy is busy with him (a decoy must not be a pause button in a fight);
- a hostile whose capsule comes within `pop_distance` 1 m of its chest knocks it over: it pops (`kHEvDetonate` b = its id:
  the guest sends `kGEvPlacedGone` and never announces it again), starers of it go on, and a **shock** decoy is a stun blast
  (`Combat::BlastAt`, `shock_radius` 2.5 m: stagger through the usual pipeline).
- `turn = true` (default off, UNVERIFIED: `ChrIns::SetYaw` is verified on the player only) also turns a starer to face it,
  again every 0.5 s while it stares.

Elden Ring's AI has no lure: nobody walks up to a decoy (that needs its AI's sound / target research, roadmap 2.8). Decoys are
kept while the puppet stops; after a load (or another player object) they are dropped (they stay in MGSV), and a guest that
re-announces one the host doesn't know (`kPlacedAgain`) is ignored -- both told to the guest with `kHEvDetonate` (as the mines'
discard): it forgets them (`kGEvPlacedGone`), so they aren't announced again at every start of the puppet. Console `decoys`,
`decoys add [m] [plain|active|shock]` (a test decoy ahead of the Tarnished, never sent to the guest), `decoys pop <id>`. Tests:
`tests/test_host_decoy.inc`.

Live pass B (2026-10-15, Gatefront, the save's E-STUN decoy 406): MGSV *throws* a decoy (LT until the arc shows, RT; pass A's LT+RT
came apart -- its presses left a gap with nothing held, and the decoy it tried was equipped mid-session): `throw: equip 406 (not an
explosive we send)`, then `placed #N: equip 406 (decoy) ... (vars slot N)` as it lands (MGSV does mirror decoys into
`placedWeapons`) and `decoys: shock decoy N set (equip 406) at ER (...)`. `npc 43111210 ... sees the shock decoy 2 14.7 m away and
stops to stare (6 s)`; a soldier shot at turned and stared at another at 11.8 m; later he walked into it: `shock decoy 3 popped:
npc 43114010 ... walked into it -> kHEvDetonate`, `stun r 2.5 m` (`poise 80 lv 3` on him), the guest `placed #3: decoy ... popped
... -> kGEvPlacedGone`. A fast travel and a death dropped the standing ones and told the guest (`4 decoy(s) set before a load ...
dropped (4 told to the guest)`). Nine were out in MGSV at once (it holds 32 placed objects), throws still worked.

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

Riding (`kGuestOnVehicle` or `kBuddyRidden`) switches the puppet to the ridden buddy's values, so it follows D-Walker's dash
and hover or D-Horse's gallop through Elden Ring's collision (roadmap 2.3): the buddy type says which (MGSV runs one buddy, and
the sandbox has no other vehicle):

| ridden | speed limit (m/s a frame's step) | kinematic step-up | kinematic body radius | camera eye height (collision rays) |
|---|---|---|---|---|
| on foot | `max_speed` 12 | `step_up` 0.5 | `radius` 0.35 | up to `eye_height` 1.6 (lower crouched/prone) |
| D-Walker (or any vehicle) | `max_speed_vehicle` 32 | `step_up_vehicle` 0.8 | `radius_vehicle` 0.35 | `eye_height_vehicle` 2.2 |
| D-Horse (`buddyType` 1) | `max_speed_horse` 32 | `step_up_horse` 0.8 | `radius_horse` 0.45 (head and tail) | `eye_height_horse` 2.4 |

The mount is logged with them (`puppet: Snake mounted (vehicle id 0x403, buddy type 1: D-Horse) -> max 32 m/s, step-up 0.80 m,
radius 0.45 m`); `pr er status` says `riding D-Horse` / `riding a vehicle`. While not ridden the horse is placed like D-Walker
(the guest's buddy pose through the anchor, ground-snapped; `RenderScene` grounds its draws by the same offset), and MGSV's own
follow brings it along. VERIFIED live 2026-10-13 with fake MGSV (`--buddy horse`, `!ride on`, the stick at its 13 m/s gallop,
Stormhill Shack): the mount line above, `buddy D-Horse (1) ... Snake RIDING`, 37.6 m asked in 3 s on the stick, 22.6 moved (the
rocks east of the grace), 8 legs of a 30 m loop at the gallop: 4 arrived, 4 stopped at rocks/cliffs, 0 unstucks; dismounted:
`placed from guest pose ... ground offset 0.02`. **With the real MGSV's ride** (`pr replay horse_ride.prrec`, the guest's recording:
mount at t 5.6, a 14-15.7 m/s gallop, dismount at 27.5; Agheel Lake North): `puppet: Snake mounted (vehicle id 0x1a00, buddy
type 1: D-Horse) -> max 32 m/s, step-up 0.80 m, radius 0.45 m`, 117.9 of 117.9 m asked moved by t 15 on open ground; Snake on
the horse drawn galloping through the grass and standing on the ground at a cliff (screenshots); the gallop then went over the
lake's cliff (`landed at 23.3 m/s -> fall damage 100%`: a fall a horse doesn't survive either) and into its wall (30 of 92 m);
`Snake dismounted`, the horse placed from the guest's pose again (`Snake on foot, placed from guest pose`) and MGSV's own
prompt to mount it on the HUD once it came back beside him. 0 warnings. `kGEvBuddyFire` is ray-traced like Snake's own shots: it is logged, drawn red and names the
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

**Changing and dismissing the buddy** (2026-10-14; MGSV's own buddy menu: iDroid > Missions > Buddy Support, or `!buddy`).
`Buddy::Absent()`: MGSV's buddy type is NONE, or the buddy this guest had out is gone (`kBuddyPresent` and `kBuddyLoading`
both cleared). Then its draws are left out of the frame (whatever MGSV still draws of it leaving) and it is not placed;
`pr er buddy` says `NONE OUT (draws left out)`. A change that held 10 frames shows in the corner (`Buddy::ChangeText`, tested):
"D-Walker dismissed -- no buddy now", "Buddy: D-Horse is out", "Buddy: D-Dog instead of D-Horse". Riding values follow the
type as before (D-Horse its own, anything else ridden D-Walker's); D-Dog and Quiet are placed and ground-snapped like
D-Walker (they aren't ridden). VERIFIED live 2026-10-14 with fake MGSV (`!buddy none|horse|dog|walker`, the fake keeps
drawing its cube when "none" as MGSV may draw a buddy leaving): the cube gone and the corner message at once, the horse
ridden with its values, the dog drawn and placed (screenshots). With the real MGSV: UNVERIFIED (the guest's side of
dismissing is the MGSV agent's).

### D-Dog in Elden Ring (`er/Dog.*`, `er/DogLogic.*`, `[Dog]`, protocol v17 `kActorDogTarget` / `kGEvDogAttack` / `kBuddyEngaged`; round E1-E2 2026-11-03)

The host half of docs/research/ddog.md (§3.1-3.3, §3.6-3.7). MGSV runs the dog -- its follow, and (the guest, `mgsv/Dog.cpp`) its run,
leap and bite on one inert stand-in soldier at the Elden Ring enemy's place; the host spots, picks, applies and draws. **On by default
since the 0.8.0 polish** (`[Dog] enabled`, `spot`, `indicator`, `bark`, `order_text` = true, also built in: a player who never touches
the ini gets it; 0.8.0's integration had shipped it opt-in for the dog's per-frame steps, docs/STATUS.md 0.8.0, ddog.md §9). Off, a
`kGEvDogAttack` is only logged, nothing is picked, marked or applied and `kHostDogLayer` is clear (the guest leaves the orders to
MGSV). `pr er "dog all off"` / `"dog all on"` for a session (not saved).

- **0.8.0 polish (2026-11-04, Elden Ring 1.17.2 alone with fake MGSV and replays of the real guest; the guest's half is GUEST.md 8.4):**
  - **Nothing while D-Dog isn't out.** `Dog::Tick` returns at once unless D-Dog is out and Snake plays (no character read, no ray, no
    marker); the paw and the why-no-pick line only with D-Dog out and LB held; BuddyOrders' LB how-to only for the buddy that is out.
    Live: `!buddy walker` / `!buddy none` and LB held 2.5 s each -> `picks 1820 (31837 rays)` before and after, no `dog:` line. The
    why-no-pick corner line now only once the right stick points at the attack entry (`dog::PointsAtAttack`: pushed over half way
    within 35° of right) -- an LB hold for Wait or the whistle says nothing.
  - **The shown pick is kept through the click** (`dog::PickLatch`, `[Dog] pick_latch_ms` 500; `dog::kClickGraceMs` 800). In 0.8.0 two
    of 16 orders were refused "out of sight" the instant the order came although the paw had shown: the 4 Hz pick, re-run at the
    order, had lost him for a few hundred ms (a pillar, a barricade post, the view moving as the stick was let go), and the guest
    looks for the pick only 350 ms after the click. Now the pick the rules last chose stays the published one (`kActorDogTarget`, the
    paw) for 500 ms after they stop choosing him -- nobody, or someone else -- while he still passes every hard rule (`LatchFits`:
    hostile by both reads, alive, rendered, not a boss with `bosses = off`, not held or lifted, asleep for Get it, 3.5-25 m, at most
    3x the cone off the centre; sight is not asked again, and since the integration's review the run is -- `dog::LatchKeeps`: with
    `clear_run` a wall, a drop or a step the dog can't take ends the latch); and a click that comes while nobody is published, within
    800 ms of the paw showing over someone who still fits, makes him the pick again (`PickLatch::Revive`; the guest's 350 ms finds
    him). Every attack order logs what the guest finds: `dog: the player's attack order 11: the pick is <him> (kept by the latch: the
    rules say nobody (... out of sight); the paw showed over him 201 ms before the click); the paw showed over him ...`; `dog
    status` counts holds and clicks. Measured with fake MGSV's call menu from the virtual pad (`!callmenu on ordered`: hold LB, right
    stick, let go, R3, the pick taken at once or within 350 ms -- the real guest's rule; `pick_latch_ms 0` = 0.8.0):
    - Stormveil's walking jars among pillars, Snake sweeping sideways 0.8 m/s (the sight line crossing a pillar's edge), 2 x 20 clicks
      each: **0.8.0: 9 of 40 refused** (3 with the paw shown <= 500 ms before); the latch alone (before the click grace): 6 of 40;
    - the lower courtyard, the view swinging ~17° off the pick as the stick is let go and the click 0.45 s later (10 clicks each):
      **0.8.0: 10 of 10 refused** (`the paw showed over 0xa...4f 450-500 ms before`); **now: 10 of 10 taken** (all through the click
      grace, the paw 201-267 ms before the click);
    - the same courtyard, a soldier in plain view, Snake sweeping (20 each): 20 of 20 taken either way; one hidden behind barricades
      (20 each): 20 of 20 refused either way (the paw never showed: correct).
  - **D-Dog's place and drawn body never follow a one-frame jump** (`er/BuddyGuard.h` `StepGuard`, `[Dog] step_guard`). The 0.8.0 run
    measured the dog's MGSV place as Elden Ring maps it stepping 0.5-0.7 m in one frame while it bit (MGSV's leap snapping its root
    onto the soldier), once 5.9 m, twice a one-frame 6-8 m blip, and ~77 m for 0.33 s after a climb-up. The guest fixes what it can
    (GUEST.md 8.4, `MaxShownSpeed`, `StageAlong`); the host's guard is the backstop and **logs every time it acts** (`render: guard
    (D-Dog as drawn): <what> -- <m> ...`, `buddy: guard (D-Dog's place): ...`), so it never hides a guest-side fault from a test:
    - per frame it takes one point -- for the draws **the dog's published place** (GuestState `buddyPos` relative to Snake's root,
      through the anchor its draws end up with, after the ground lift, the floor shift and the Wait pin; until the integration run
      with both games it was the median of the dog's draws' own centres, see below), for the placement (`Buddy::Position`: the pick,
      spotting, Bark, the ground ray) the mapped `buddyPos` -- and its own motion (the median of its last three steps);
    - a **snap** (0.3-1.5 m off its own motion in a frame) is taken back and given out over the next frames (`settleS` 0.1 s): MGSV's
      leap looks like a leap; a **jump** (1.5 m and more, plus 15 m/s x a long frame's extra time) is **held** -- the dog stays where
      it was -- until it comes back (within max(1 m, half the jump) of where it was held, but never further than 2 m: it was never
      drawn there; review: a 60 m jump settling 20 m off counted as "came back" and glided there in view), or is taken as a real move: at once where
      neither the old nor the new place is in view (the guest's leash, which moves the dog out of MGSV's view), eased in at most
      0.5 m a frame after 100 ms when it is within 10 m, and a farther one in view after 2 s at the latest (the placement: after
      500 ms, never eased); every 60 Hz frame the drawn point moves at most 0.5 m (`maxStep`) but where it is taken out of view;
    - it starts over when the puppet starts (`Puppet::StartEpoch`), when Snake is put more than 5 m away in a frame (a grace, a
      load, a `call setpos`), a new guest session, 250 ms without the dog's draws, a ladder (the buddy isn't drawn), and moves with
      Elden Ring's origin (`WorldShift`). A 1.5-5 m re-anchor (Elden Ring shoving the player, a grab) no longer starts it over
      (review: the mapped dog's jump with the anchor was drawn at once): it is held and eased like any jump.
    - also (all buddies): while Snake's body is turned for a staged move (`Puppet::StagedDrawAnchor`, a climb-up / vault), the buddy's
      draws are put back through Snake's own anchor (C = A_staged^-1 * A_own on its matrices) -- the staged anchor had swung them
      round him with his body.
    - `pr er buddy` adds both guards' counts: frames measured, steps over 0.5 m raw and drawn, the largest of each, snaps, holds and
      how they ended; `buddy guard reset` zeroes the draws' counts.
    Live: fake MGSV's `!dog blip <dx> <dz> [frames]` (the dog's place and draws off for N frames): a 7 m one-frame blip -> `jump held
    -- 7.00 m`, `jump came back -- 7.00 m after 16 ms`; 6 m for 2 frames -> back after 33 ms; 77 m for 20 frames (the climb-up case) ->
    held 333 ms, came back; a 0.8 m one-frame step -> `snap spread`; over those 501 frames 7 raw steps over 0.5 m (largest 77 m), **0
    drawn** (largest 0.19 m). Frames: a 77 m jump for 1.25 s -- with the guard the dog stays drawn beside Snake, without it it is gone
    (drawn 77 m away). **Replays of the real guest**: `ddog_kill_080.prrec` (the knife's run, leap and pin; 2671 frames): 14 raw steps
    over 0.5 m (largest 0.78 m), 0 drawn (largest 0.50 m: the limit), 34 snaps spread; `ddog_attack.prrec` (the battledress's run,
    leap and pin, then Wait, Snake running off and back, the whistle; 2910 frames): 12 raw over 0.5 m (largest 2.27 m, taken out of
    view during the wait), 0 drawn, the bite's 0.62-0.81 m snaps drawn as 0.04-0.15 m steps.
  - **A waiting D-Dog keeps its Elden Ring spot** (`er/BuddyGuard.h` `WaitPin`, `[Dog] wait_pin`). What each side does:
    - the **guest** keeps the waiting dog's MGSV place through Snake's treadmill jumps (GUEST.md 8.4: its Elden Ring place stays put as
      long as Elden Ring lets Snake go where MGSV's Snake goes), and -- with its `[Dog] WaitKeepsPlace`, added by the guest's side of
      this polish (GUEST.md 8.4) -- adds the slip it sees in the Elden Ring characters around Snake to what Elden Ring is shown of
      the waiting dog (its published place and draws; moving MGSV's dog itself ended MGSV's Wait);
    - the **host** knows Snake's slip exactly -- his Elden Ring step minus his MGSV step (the odometer, as the puppet consumes it)
      mapped to Elden Ring, summed since the Wait (9) -- and takes back **only the part of the dog's drift that slip explains** (the
      drift's projection on the slip, at most the slip): a guest that already moved the dog leaves ~0 to correct, so the two never
      add up, and while the guest's correction lags the host fills the gap. The dog's own moves off that line pass through; 1.5 m of
      them and the wait is over (MGSV ended it); the whistle (8) or DD (16) end it too, and so does an attack order MGSV takes
      (`kGEvDogAttack` Ordered, `Buddy::OnDogAttack`; a refused one leaves the dog waiting -- review: the pin kept the bite drawn off its
      target); the correction then eases out at 2 m/s, or with the dog's own run (0.75 of its step a frame) when that is faster.
      MGSV's staged moves freeze it (their places mean nothing) until 1 s after; Snake put more than 5 m away in a frame (a teleport)
      drops it (`Buddy`'s slip), and a slip built up past 40 m against a wall lets go eased (it was dropped in one frame); it moves
      with Elden Ring's origin. With `[Dog] enabled = false` neither the pin nor the step guard runs (review: the kill switch said
      "as in 0.7.0"). Applied to the placement (so the pick, the marks and the ground ray use the spot) and, unsmoothed, to the
      dog's draws (`Buddy::WaitShift`).
    Live (fake MGSV's `!order 9`: its dog keeps its place in the odometer frame and moves nothing by the slip; Snake run 2 s at 3 m/s
    into the wall of the jars' courtyard): **pin on -- Snake asked 6.31 m, moved 2.02 m (slip 4.30 m), the waiting dog stayed at
    (-3.13, 2.89) to the centimetre; pin off -- slip 4.24 m, the dog's spot moved 4.25 m.** The real guest's `ddog_attack.prrec` Wait
    (Snake running off and back through the lower courtyard's barricades): `D-Dog waits`, the correction reached 5.21 m, `called back
    (order 8) -- its Wait correction 5.21 m eases out`.
  - Live as well: **Snake's CQC grab on the enemy the dog holds** -- `dog attack busy` on a soldier 1.4 m from Snake (`cqc: D-Dog holds
    npc 30000114 ... for 6.0 s (no attack / no move; bits 0x30 ours)`, ChrIns+0x538 = 0x30), fake `!grab grab` 0.7 s later: `cqc:
    Snake takes npc 30000114 ... over from D-Dog's hold`, `dog: done with ... (hold; the hold ended)`, still 0x30 (one hold, one owner);
    `!grab release`: `let go (Snake let go)`, 0x00, and 0x00 6 s later (nothing left set). A kill through the dog's effect on two
    soldiers next to Snake: `bullet ... landed after 39 ms` / `did not land within 300 ms; hp fallback -265` -> both dead. The paw over
    a soldier whose neighbour, a team-30 `81100014` lying 1.2 m from him, was nearer the centre (2.8° vs 4.1°): `not hostile (never a
    candidate)`, the paw over the soldier (frame), the LB how-to in the corner. Every hit of the session was on a soldier (the log
    names each); a lying `81100014` lost 358 HP in the lower courtyard to Elden Ring's own scene, none of ours.
  - **Not done:** a real Elden Ring dog or wolf as the target (none loaded around Stormveil's Rampart Tower and lower courtyard: the
    pack found there, `44910014` x7 + `44907014` (1.0 / 2.9 m tall), are pot-shaped walkers, not dogs -- picked and spotted like any
    hostile; Limgrave's wolves need a grace trip); `bosses = stagger` is unit-tested only (`TestDogBossStagger`: a boss is pickable then, one stagger -- no
    damage, no hold, no sleep -- then nothing for 20 s, then one again; with `off` never picked, nothing applied).
  - **The integration run with both games (2026-11-04, real MGSV 1.0.15.4 + Elden Ring 1.17.2; docs/STATUS.md "0.8.0 polish"):**
    - **The draw guard watched the wrong point.** With the real dog, the median of its draws' matrix means (skinning matrices: the
      bind pose's offsets swing as the dog turns, and the set of draws changes) jumped 1.5-6 m in a frame while the dog's published
      place moved under 0.25 m -- 74 "jumps held", 15 taken out of view and 135 snaps in the first 20 minutes were all the estimate's
      (the guest's per-frame trace had no such step). Holding such a "jump" moves the real body by it: the guard's offset changed by
      more than 0.5 m in a frame 125 times in the first 10-minute run (up to 6.4 m, in view). The guard now watches the published
      place (`RenderScene::GroundBuddy`: GuestState `buddyPos - rootPos` through the anchor the buddy's draws end up with; the
      estimate only while the guest has no place), which the guest already moves at most 14 m/s and with its draws. The second
      10-minute run: 0 guard offset steps over 0.5 m but where `call setpos` moved Snake himself.
    - Review fixes (commit "Review fixes for the D-Dog polish"): the latch and the click revive re-check the clear run
      (`LatchKeeps`); a held jump comes back within 2 m at most; the guards start over only when the puppet starts
      (`Puppet::StartEpoch`), not at a 1.5-5 m re-anchor; the pin ends at an attack order MGSV takes (`Buddy::OnDogAttack`) and eases
      with the dog's run; a 40 m wall push lets go eased; `step_guard` / `wait_pin` only with `[Dog] enabled`; the drawn body's lift
      and floor shift are measured where the renderer's guard draws it (`Buddy::DrawGroundOffset` / `DrawGroundShift`, the unguarded
      place, one more ray only while the placement guard holds a move).
    - `buddy trace on|off`: one CSV line per frame D-Dog is drawn (`PhantomRingBuddyTrace.csv` next to the DLL: the raw and drawn
      place, the guard's act, raw / drawn in view, guest and buddy flags, Snake's Elden Ring place, the Wait pin's shift, the lift, dt,
      and the old draws' estimate for comparison) -- with the guest's `!dog placetrace on` the measure of every frame (docs/TOOLS.md).

- **0.8.0 (review fixes and the integration run with both games, 2026-11-04):**
  - HostState **`kHostDogLayer`** (v17) is set while `[Dog] enabled`: the guest takes the player's attack orders over only then, else
    they are MGSV's own (0.7.0). The guest publishes **D-Dog's style every frame** in buddyFlags bits 8-15 (`proto::BuddyDogStyle`):
    the pick is made for it before any order ("Get it" takes sleepers from the first order on; `dog: D-Dog's style is ... (its
    equipment, published by the guest)`).
  - **A boss** is `Cqc::IsBoss(npcParam, maxHp)` everywhere (the pick, the bite, Bark, the dog's hold, CQC's grab, decoys, the box
    poster, the Fulton, `kActorBoss`): CQC's HP rule **or Elden Ring's own mark of a boss, his NpcParam row's `isSoulGetByBoss` bit**
    (`params::kNpcBossBits` 0x15D bit 0: all 315 boss rows of this regulation -- Godrick 47500014, the catacombs' and evergaols' 9xx rows
    of 350-1500 HP -- and no soldier; `test_host_combat` checks it on the real regulation). An NpcParam row that can't be read counts
    anything bigger than an ordinary enemy (Bigness > 0) as one. Live: `cqc` -- `NpcParam's boss bit (204 seen, 0 rows unread)` with
    Godrick within the ActorTable's radius; the pick lists him `a boss` at 147 m (never approached).
  - The bite never lands on one a **Fulton balloon** lifts or **Snake's CQC** holds (`connected on ... a Fulton balloon has him:
    nothing done`; live on a fetch: the yank's Connected found him lifting), nor while D-Dog isn't live (`while D-Dog isn't live here`);
    anything a Connected or a console bark left while not live is dropped at the next frame. A second busy bite on the enemy the dog
    holds keeps the hold (it used to let him go at once). A dog **kill or wound whose hit bullet misses** (a raised shield, a turn) is
    dealt through the HP route (`HitEffect::landOrHp`: `hp fallback -N (D-Dog's bite connected: its damage is dealt anyway)`; nonLethal
    still holds) -- live, a kill bullet on a soldier charging Snake had missed and left him unhurt.
  - LB counts as the call menu's only while neither Elden Ring's nor MGSV's menu (the iDroid pages with LB) nor a vehicle has the pad;
    no failure text after the player's own whistle, Wait or DD (`dog::QuietEnd`, 2.5 s) or while not live; `the pick (kActorDogTarget)
    is nobody (<why>)`; the floor-near-Snake search only within 30 m of Snake (a horse left 150 m away cost ~300 rays a frame).
- **Live** only while D-Dog is out (`buddyType` 2, `kBuddyPresent`, not `Buddy::Absent`) and Snake plays (no load, death, cutscene).
  Every frame `Dog::Tick` (FrameBegin, after `Cqc::Tick`) reads the 64 nearest characters once.
- **Never a friend.** Every rule below takes only characters that are hostile by *both* team reads: the ActorTable's (an `EnemyIns`
  of another raw team byte, ChrIns+0x6C, that the team relation table makes the Tarnished's enemy) and every hit's (`GetTeamType`,
  `Combat::HostileChr`). NPC phantoms (`PlayerIns`: Rogier, summonable NPCs, NPC invaders) are never even looked at. A merchant, a
  quest character, a spirit summon, a neutral animal (Stormveil's team-30 `81100014`s) is no candidate, smelled, bitten or barked at.
- **Spotting** (`spot`, every 0.25 s): `dog::Spotter` -- hostile, living, rendered characters within `spot_range` 40 m of the dog
  (horizontally; the dog's placement, `Buddy::Position`) and `spot_height` 8 m above or below it; no line of sight. Each one gets
  `Marking::Provisional(handle, until)` (until = now + `spot_linger` 10 s, renewed while he stays in range): a **provisional mark** --
  the binocular mark's triangle drawn **hollow** (`marking::kHollow`, the marker shader's band), his distance only within 10° of the
  view's centre; it fades over its last 0.5 s and at his death; the binoculars still mark him for good; a new world forgets them. When
  the spotted set gains someone: "D-Dog: Found enemy" in the corner, "(N nearby)" with several, at most every `spot_text_every` 8 s
  (`dog::FoundText`; a gain inside the quiet time is said when it ends if someone is still spotted).
- **The pick** (`dog::Pick`; 4 times a second while LB is held and 2 s after, else once a second): hostile, alive, rendered, not on a
  Fulton balloon, not a boss (`Cqc::IsBossHp`, unless `bosses = stagger`), not held (`Cqc::IsHeld`: Snake's or the dog's hold), asleep
  for Get it (the style the guest last named: `kGEvBuddyOrder` pad[0]), `target_min` 3.5 - `target_range` 25 m from Snake, within
  `target_cone_deg` 10° of the camera's axis (his body counts); marked ones first (`target_marked_first`), then the one nearest the
  centre; at most three get rays: the camera's sight line (`CastRay`, `marking::RayClear`) and the **clear run** (`clear_run`): ground
  samples every 0.5 m from the dog's feet to 0.7 m short of his (Snake's ground filter, `Puppet::CastWorld`, from 2 m over the line to
  4 m under it), refused for a step over `run_step_max` 0.8 m between samples, a sample without ground (a drop) or a knee-high (0.5 m)
  ray along the line that hits something (a wall). The winner's handle is `Dog::TargetHandle()`; `Actors::Publish` sets
  `kActorDogTarget` on that one record only, and only if the record is `kActorHostile`, alive and not `kActorBoss` (checked again at the
  boundary). `Actors` now also sets `kActorBoss` on hostile bosses (CQC's rule).
- **The paw** (`indicator`): while LB is held, an amber paw (`marking::BuildPaw`, glyph 11 of the marker shader: a pad and four toes,
  dark-edged, through walls; `RenderMarkers::Owner::kDog`) over the pick's mark. LB held 0.6 s with no pick: one corner line per hold
  saying why the enemy in view isn't taken (`dog::NoPickText`: "D-Dog can't run to him straight (a wall in the way)", "he is out of
  sight", "he is too close -- CQC him", "won't take on a boss", ...), else "D-Dog: look at an enemy (25 m)".
- **`kGEvDogAttack`**: Ordered is logged; **Connected** re-checks the handle (in Elden Ring's list, hostile by both reads, alive -- else
  `dog: connected on ... NOT a hostile Elden Ring enemy ...: nothing done`, a warning), one effect per bite (a second Connected within
  1 s is ignored), then the style's effect (docs/COMBAT.md §7b) through `Combat::HitCharacter` from 0.8 m on the dog's side of him (v1
  mapped: a raised shield faces the dog), queued for the bullet row like CQC's hits (`cqc::WhenToFire`); Released / Lost end the
  engagement (the dog's hold let go); Refused says why in the corner ("D-Dog: no enemy in sight", or the pick's reason). An unknown
  style (0xFF) is the one the player's command named (11-15, within 30 s), else the last known, else busy. The guest's `kBuddyEngaged`
  going off 1.5 s after Connected ends it too.
- **Bark** (`bark`; `kGEvBuddyOrder` a = 10, b = 2): `dog::HearsBark` -- the active decoy's rule (`decoy::Notices`, heard within
  `bark_range` 20 m of the dog, no cone, no ray; not one within 4 m of Snake and nearer him than the dog), never a sleeper, a boss, one
  held or lifted: each stares `bark_stare` 5 s (ChrIns+0x538 no attack / no move, only the bits we set go back), `bark_cooldown` 15 s.
- **Holds** (`Cqc::HoldFor` / `LetGo` / `DogHolds`): busy's hold uses CQC's own bookkeeping, a second slot beside Snake's grab, so a dog
  hold and a CQC hold never stack on one enemy -- one owner of his bits, put back once: Snake's grab of the one the dog holds takes it
  over; HoldFor refuses one Snake holds, a boss, one on a balloon, one let go less than `hold_cooldown` ago (then the bite is a flinch);
  no `leave_distance` rule; it ends at `busy_hold`, his death, his leaving the world or the balloon.
- **The floor near Snake** (all buddies, not while ridden; `Buddy::FloorNearSnake`, `DrawShift`): where Elden Ring has no ground within
  8 m under the mapped buddy (beside a tower top, a cliff's edge), it no longer floats at MGSV's height: the floor at Snake's height
  (±1.5 m) under it, else the nearest one on the line toward Snake (0.5 m steps); `RenderScene::GroundBuddy` moves its draws there
  (smoothed like the lift; the anchor matrix's x/z columns turn the ER shift into the draws' space). `pr er buddy` counts them.
- Orders in the corner (`order_text`, `er/BuddyOrders.cpp`): D-Dog's commands 8-16 ("D-Dog: Keep 'em busy -- he pins the enemy you look
  at", "D-Dog: Get it -- he fultons the sleeping enemy you look at", ...); the LB how-to per buddy ("D-Dog orders: ... -- look at the
  enemy first"). Without `order_text` a dog order is "Buddy order N" as before.
- Console: `dog [status | on | off | all on|off | pick | test boss | style <s> | attack [busy|kill|knockout|wound|fetch] [0xHANDLE] |
  release | bark | set <key> <value>]` (keys include `pick_latch_ms`, `step_guard`, `wait_pin`; `buddy guard reset`) -- `pick` lists every character around with the reason he is or isn't the pick (NPC phantoms
  "never a candidate"); `test boss` the same with every hostile one treated as a boss; `attack` / `release` a Connected / Released as the
  guest would send it (no `kBuddyEngaged` to follow then). No new Elden Ring address. Tests: `tests/test_host_dog.inc` (in
  test_host_combat), `test_host_marking.inc` (provisional marks, the hollow triangle, the paw), `TestBuddyOrders`; 0.8.0 polish:
  `TestDogLatch` (the latch, the click grace, `LatchFits`, the attack entry), `TestBuddyGuard` (a run untouched, the leap's 0.59 m snap
  spread, the 7 m / 77 m blips held and back, a 5.9 m snap eased in, the leash taken out of view, a far move taken in view after 2 s,
  a long frame, the placement's rules), `TestDogWaitPin` (the slip taken back, a guest that moved the dog itself not corrected twice,
  a lagging one filled in, staged moves, the end, the ease, a teleport, the origin shift), `TestDogBossStagger`, and test_host's
  `[Dog]` defaults (on) and keys.

**Live 2026-11-03** (Elden Ring 1.17.2 alone, `--er-headless 3840x2160`, fake MGSV `--buddy dog` and `pr replay` of the real
`dog_follow.prrec` / `idle_walker.prrec`, the player's save at Stormveil; `combat set hurt_mult 0` throughout; the saves restored
after: 8 of 8 files equal by sha256):
- spotting: `dog: smells npc 42101014 ... 25.9 m from the dog`, `smells npc 30100114 ... 28.2 m` -> "D-Dog: Found enemy (2 nearby)"
  (frame: the corner line); frames: two hollow red triangles over a tower top 8 m above Snake, others through walls, "16m" / "18m"
  near the centre only; with the real dog's placement (replay): `smells ... 35.8 m`, `37.8 m` -> the same corner line;
- the pick and the paw: lower courtyard, `dog pick`: `PICKED npc 30001014`; with LB held the amber paw over him (frame); the guest
  (fake) read `kActorDogTarget` on 4 handles in the session, all hostile soldiers / a warhawk, never a non-hostile one;
- never a friend: a Stormveil team-30 `81100014` lying 1.2 m from a soldier, 1.5° off the centre at 15.6 m: `not hostile (never a
  candidate)` while the soldier at 5.1° is the one checked; the Rampart Tower room: the team-0 armoured figure (`30100214`) 1.1° off the
  centre at 5 m -- no pick, no paw, "D-Dog: look at an enemy (25 m)" (frame), Rogier's phantom (team 26 `PlayerIns`) never looked at;
- refusals seen: `too near (CQC's)`, `out of sight` (a wooden siege engine in the way: `call rayall` showed two map bodies between),
  `no clear run (a step)` (a barrel 1.2 m high 1 m before a soldier; `run_step_max 2` took him), `no clear run (a wall)` (rubble,
  barricades), `awake (Get it takes sleepers)`, `a boss` (`46000014` 3148 HP, `47500014` 6080 HP, listed from 57-141 m: never
  approached); `dog test boss` on a warhawk in the cone: `a boss`, `a bite ... as a boss: refused (a boss)`. "D-Dog can't run to him
  straight (a wall in the way)" in the corner with LB held (frame);
- the bites, Bark, Get it: docs/COMBAT.md §7b; holds and decoys: a dog-held warhawk was `held (CQC)` in the pick, an active decoy's 4
  stares left him alone;
- the floor near Snake: Snake on a rooftop with the dog's spot past the parapet: `drawn shifted 0.50 m` (`389 moved toward Snake`),
  D-Walker the same (frame); the real D-Dog (`dog_follow.prrec`) drawn on the wooden floor beside Snake, the real D-Walker
  (`idle_walker.prrec`) beside him (frames); D-Walker's order and how-to unchanged ("D-Walker: Search Mode ...").
- the committed build (Elden Ring's characters read only when spotting, the pick, the paw or a hit needs them) once more, a third
  launch: "D-Dog: Found enemy (2 nearby)", the paw over a warhawk on a tower top 25 m off (`target_range 35`, `clear_run off` for it;
  frame), its busy bite (flinches at +1.65 / +3.17 / +4.65 s, let go at 6.0 s), Bark (`bark_range 35`: 2 of 2 stare); 57 picks, 59 rays.
- Not live: the guest's own dog (its run, leap and bite; `kBuddyEngaged` from it), `bosses = stagger`, Snake's CQC grab taking over
  the dog's hold, a real Elden Ring dog (Stormveil has warhawks: `4210xxxx`), the ER 2.7.1.0 exe (no new address, nothing to check).

### Fulton: MGSV's recovery of an Elden Ring enemy (`er/Fulton.cpp`, protocol v12 `kGEvFulton`)

The guest's ActorProxy stands an MGSV soldier in for an Elden Ring character (docs/GUEST.md 20); when Snake fultons it, MGSV's
own balloon goes up and the guest sends `kGEvFulton` with that character's handle (`pad[0] | pad[1] << 32`), where the proxy
stood (`v0`, MGSV world) and `a` (1 the balloon went up, 0 it failed: nothing happens). The host:
- finds the character by its handle among game::Characters -- that one while he lives, else nobody (review 2026-10-31: a dead
  or unloaded handle used to pass the balloon to the nearest living hostile within 3 m, awake or a boss); only with no handle
  at all the nearest living one within 3 m of where the proxy stood -- only an enemy of the Tarnished by Elden Ring's team
  relation table (the rule every hit uses: live, a "nearest" without it took an invisible team-0 character standing at the
  player's grace, and the grace's prompt went); and only while he still sleeps (`Combat::Asleep`) and is no boss
  (`Cqc::IsBossHp`, CQC's rule): refused otherwise (`fulton: ... is awake now` / `is a boss`; `pr er fulton` counts them).
  `er/Actors.cpp` never flags a boss `kActorAsleep`, so MGSV offers no Fulton for one;
- holds it (ChrIns+0x538 bits 4/5 no attack / no move, +0x1C4 bit 5 no gravity) and lifts it under a balloon for 2.2 s
  (`Fulton::LiftAt`, tested): the balloon inflates over 0.7 s while he is lifted 0.25 m, then 1 m in 0.6 s, then yanked up
  ~18 m; drawn with the world markers (an olive-grey ball and a dotted line to his back), moved with Elden Ring's origin
  shifts; "Fulton: the enemy is on its way to Mother Base" in the corner;
- then takes it out of the world with no death, no drop and no kill: ChrIns+0x538 bit 12, fromsoftware-rs `character_disabled`
  ("no updates, no rendering, no physics"; [V-src], VERIFIED live: the soldier vanished and stayed gone, no crash).
  `game::Characters` leaves disabled characters out, so nothing shoots, targets, marks or proxies it afterwards;
- keeps its handle until Elden Ring brings its enemies back (a rest at a grace, a respawn, a load): one that turns up again
  meanwhile (its block reloaded) is disabled again (`turned up again ... out of the world again`; seen live once). After a
  rest it is back like any enemy;
- gives the Tarnished the runes it would have dropped (`[Fulton] runes`, on): its NpcParam `get_soul` (+0x2C; the table bound by
  name, struct and row size like the others) onto PlayerGameData (PlayerIns+0x580, RTTI `CS::PlayerGameData`) `rune_count`
  +0x6C and `rune_memory` +0x70 (fromsoftware-rs; read live: level 20 at +0x68, runes 0, memory 25936 on the player's save),
  after sanity checks (else off for the session, logged). No Golden Scarab or New Game+ scaling. VERIFIED live: `fulton: +114
  runes for npc 30003214 (its NpcParam getSoul): 0 -> 114`; Elden Ring's menu showed 114 at once, its in-game HUD counter only
  at its next update (a direct write skips its pickup animation).
Console: `fulton [status | nearest [fail] | 0x<handle> [fail] | forget]` sends the event as the guest would; fake MGSV
`!fulton [any|0xHANDLE] [fail]` picks from the ActorTable. VERIFIED live 2026-10-14 (fake MGSV, Stormveil, Stormveil soldiers
npc 30003214 / 30001014): `fulton: the balloon goes up on npc 30003214 (handle 0xa00000010000033, hp 221/221)` -> 2.19 s
later `is out of Elden Ring's world (after its lift; ChrIns+0x538 |= 0x1000 character_disabled: no death, no drop)`; screenshots:
the balloon over the soldier's head on its line, then the soldier hanging under it well above the trees, then gone; he never
came back in `chr`. A soldier mid-swing finishes the swing (the hold stops new actions only). The ActorTable's hostile flag
now uses the same team relation (it marked any EnemyIns of another team, the invisible team-0 one too).
**Asleep** (`kActorAsleep`, which the guest's Fulton anchor needs: docs/GUEST.md 20): `Combat::Asleep` -- an active SpEffect of
stateInfo 436 (a sleep build-up row) next to one of stateInfo 434 (row 54, "helpless"; 434 alone is a stance break). Found live
2026-10-14 by watching a Stormveil soldier's effects while our ladder slept him: 25019 (436) and 54 (434) on from 0.9 s until he
woke, both gone with it; CSChrResistModule's per-status "procced" word stays 1 afterwards (no use). VERIFIED live: `combat targets`
shows `ASLEEP` and the ActorTable `Hostile|Asleep` from 1 s after the dart until he woke 51 s later (then `Hostile` only); fake MGSV
`!fulton` (now the nearest sleeping hostile record; `awake` / `any` for others) sent kGEvFulton for him and the host lifted him out.
**VERIFIED live end to end with the real MGSV** (combined run 2026-10-14, Gatefront, a Limgrave shield soldier npc 43114010):
two darts (`sleep +136` twice, resist 226) -> the guest's `proxy: soldier 0x450 asleep for Elden Ring character
0x3c2a250017300018 (MGSV offers its Fulton beside him)` -> Snake walked up: MGSV's Fulton prompt ("100%", the balloon, x48) and
its sleeping-soldier "ZZZ" marker over the Elden Ring soldier -> Y held 2.5 s -> 5.3 s after the press `proxy: fulton ...
succeeded (MGSV's own Fulton on its proxy) -- kGEvFulton a=1` -> `fulton: the balloon goes up on npc 43114010` -> 2.2 s later
`out of Elden Ring's world` -> `+64 runes` (the menu's rune counter 0 -> 64, "Fulton: the enemy is on its way to Mother Base" in
the corner, MGSV's own counter x48 -> x47). The first two tries failed for the timing alone: the soldier woke (Elden Ring's
sleep, ~50 s) while the script walked up, and once a second Gatefront soldier killed Snake beside him.

### Marking: MGSV's binoculars on Elden Ring's characters (`er/Marking*`, `[Marking]`, protocol v13 `kGuestBinoculars`)

ROADMAP 2.6, without proxies: the guest sets `kGuestBinoculars` while MGSV's PlayerStatus `BINOCLE` is on (`mgsv/Frame.cpp`, from
the `kLBinocle` bit `GuestScript` PR.Flags already reads for `kGuestFirstPerson`). Elden Ring's camera is then the binoculars'
view, zoom included (`er/CameraDriver.cpp` copies MGSV's FOV). Each frame (`Marking::Tick`, after the guest's events):
- the characters around (`game::Characters`, `max_characters` nearest, never the Tarnished; `targets = hostile` keeps the ones
  the ActorTable flags `kActorHostile`; `rendered_only`: not the ones Elden Ring doesn't render, ChrIns+0x1C5 bit3 clear --
  ballista/arrow-trap dummy shooters, characters an event hides -- nor ones with no max HP) within `range` (150 m) of the camera
  whose body (0.6 m around the chest, 1.2 m over the feet, or his own hit radius when larger: the physics module's capsule, the
  offsets er/Combat.cpp reads) is within the **cone**: `cone` (0.12) of the camera's half vertical FOV, never under
  `min_cone_deg` -- the same part of the screen at any zoom, and a close one filling the view counts;
- the six nearest the centre get a **line of sight** ray (Elden Ring's `CastRay` from the camera to the chest, the puppet
  ignored, `[Puppet] ray_filter`; `marking::RayClear`: a hit within 1 m of the chest still counts, and so does one on his own hit
  capsule (+0.3 m) -- a troll's or a boss's is metres wide, should the ray stop on character bodies); without the ray cast
  (unresolved) marking goes on without it (warned once);
- **dwell**: the seconds looked add up while he is in the cone and in sight, and ebb away twice as fast when he isn't; at `dwell`
  (0.5 s) he is marked (`marking: marked 0x... (npc N, hostile) at D m`);
- a marked character keeps his mark until the next **world**: `marking::LoadWatch` forgets every mark after a load screen of
  0.5 s or more (fast travel, a death, the title, another save -- placed characters come back with the same FieldIns handles,
  runtime spawns may reuse them; MGSV's marks last a mission) or when the Tarnished's ChrIns changes. Within a world one not in
  the list (unloaded, fultoned, not rendered) keeps it and shows again when back; a rest at a grace keeps it (Elden Ring's
  respawns keep their handles); one seen dead (or at 0 HP) fades over `fade` (1 s) and is forgotten; at most `max_marked` (the
  oldest goes).
The mark (`marking::Build`, the world markers of er/RenderMarkers.h, owner `kMarking`): a triangle pointing down with its tip
`above` (0.35 m) over the top of his own hit capsule (`height`, 2.2 m over his feet, without one: "Real heights" below), red (hostile) or white, dark-edged, `size` (0.012) of the screen height at any distance and FOV;
his distance from Snake (the puppet) in whole metres as seven-segment glyphs over it (`text_size` of the triangle;
`distance = always | centre | off`, centre: only within 10 degrees of the view's centre); a fresh mark's triangle pops (1.6x,
0.3 s; the number stays put). Both are new marker shapes in the marker pixel shader (`kTriangle` 3, `kGlyph` 4: signed
distances, a pixel-wide soft edge from `fwidth`), drawn with a depth slack of 10^6 m: through walls, like MGSV's. None for the
Tarnished, in Elden Ring's menus (a grace's too) or talks (`interacting`), in MGSV's iDroid/pause menu (`kGuestInMenu`, as
Fulton's markers), cutscenes, loads, death. Elden Ring's HUD hiding does *not* hide them: with its "Auto" HUD it hides out of
combat, exactly while Snake scouts (the same pitfall `AutoHud` handles in er/Host.cpp). Nothing in Elden Ring is written:
no new addresses (the capsule offsets are Combat's).
Console: `mark [status | binoculars on|off|auto | nearest | clear]` -- `binoculars on` marks as if the guest's flag were set
(tests with fake MGSV, or the real one without RB); fake MGSV `!binoculars on|off`; status counts the blocked rays and the
characters skipped as unrendered. Tests: tests/test_host_marking.inc (cone, big bodies, dwell, rays and `RayClear`, deaths,
caps, `LoadWatch`, layout and the pop); the shaders compile in test_render. **Live pass A (2026-10-14):** the guest's
BINOCLE flag (`binoculars up/down`), a Stormveil soldier `marked ... (npc 30001014, hostile) at 2 m` 0.48 s after the
binoculars came up (31 rays, 0 blocked), his red triangle and distance in Elden Ring's frame; `mark nearest` draws a white one.
Still unverified: foliage vs. the ray filter, giants, fast travel as a load.

**Real heights** (0.7.0; `marking::MarkHeight` / `AimHeight`, `[Marking] above` 0.35, `height` 2.2 now the fallback; console
`mark handle <0x..>` marks one character): a fixed 2.2 m over the feet put the triangle inside a troll, a giant or a boss, and the
cone and the ray aimed at a fixed chest. Now each character's own hit capsule (NpcParam hitHeight / hitRadius as the physics
module has it, the one Combat reads; cached per handle) sets both: the tip `above` m over the capsule's top, the aim at 0.65 of
its height. A capsule that can't be read or isn't sane: the old fixed heights (`mark` counts both). Live 2026-11-03 (Stormveil):
`marking: 0xa0000001000001b (npc 30003214) marked -- its capsule 1.90 m tall, r 0.40, the mark's tip 2.25 m over its feet`;
30100814 2.10 m -> 2.45; 36600014 1.77 -> 2.12; 81000014 2.00 -> 2.35; the large 46400014, 4.50 m tall (r 1.80) -> 4.85 m (it was
at 2.2 m, its waist). Frames: the marks with their distances (27 m, 70 m) drawn through the walls between; the characters
themselves were behind those walls, so the height over a head is from the log, not seen.

### Phantom Cigar = Elden Ring time skip (`er/TimeSkip.cpp`, `[TimeSkip]`, protocol v13 `kGEvTimeSkip`; live pass B 2026-10-15)

Roadmap 2.7. While Snake smokes MGSV's Phantom Cigar, the guest streams the game minutes MGSV's time-lapse runs through
(docs/GUEST.md 18.16): `kGEvTimeSkip` with a = minutes, a few every quarter second. The same game time passes on Elden
Ring's clock:
- **The write** is the one VERIFIED with `poke` (Lighting from Elden Ring, below): `WorldAreaTime` +0x2C minute and +0x30
  second, then +0x28 hour (u32 each; the hour write applies the three). No new address: `WorldAreaTime` is the verified
  table entry `Game::TimeOfDay` already reads (+8 packed h/m/s).
- **Smooth:** `phantom::timeskip::Pacer` (`common/TimeSkipLogic.h`) spreads every skip: each `step_ms` (100) it hands out
  `pending x step_ms / smooth_ms` (1000), at least a minute -- a 6 h skip in ~45 writes over ~4 s, big steps first; the
  guest's small streamed skips go out as they come, so Elden Ring's sky follows MGSV's time-lapse. `smooth_ms = 0`: one write.
  Our own cursor carries the time between writes (a write may show a frame later), and takes Elden Ring's reading when it is up
  to 10 min ahead of it (its own clock ran on). Whole days are dropped. Round midnight is just the dial going round.
- **When:** only in a loaded world (`Frame::inGame`, not `loading`): a skip that arrives during a load waits for the world.
- **Log / corner:** `time skip: MGSV's Phantom Cigar -- Elden Ring's clock from 12:00 on`; 3 s after the last write `time
  skip: Elden Ring's clock 12:00 -> 18:30 (+6 h 30 min, N writes)` (N: this skip's writes; `timeskip` shows all) and the corner "Phantom Cigar: 6 h 30 min passed (12:00 ->
  18:30)".
- **Console:** `timeskip` (status: Elden Ring's clock, events, minutes, pending, writes), `timeskip <minutes>` (as a
  kGEvTimeSkip: the host side tested without MGSV), `timeskip clear`.
- **Night:** Elden Ring's sky and light follow the clock at once (seen with `poke`). Whether night-only characters (Night's
  Cavalry, Renna at the Church of Elleh) appear without an area reload is UNVERIFIED; Elden Ring's own "pass time" at a grace
  reloads the area.
- `[TimeSkip] enabled = false` leaves Elden Ring's clock alone (the event is logged once). On by default: it only acts on
  kGEvTimeSkip, which the guest sends with its own `[Cigar] TimeSkip` on (on since live pass B), or on the console command.
- Live pass B (2026-10-15): `timeskip 900` moved 07:23 -> 22:23 in 53 writes (~8 s); smokes moved Elden Ring's clock 12:14 ->
  18:02 (+5 h 48 min, 84 writes), 18:57 -> 02:14 (+7 h 17 min, 98 writes) and 04:10 -> 16:18 (+12 h 2 min) with the sky following
  (the frames go from noon to dusk to a moonlit night); at night, after a fast travel, Renna stood at the Church of Elleh.

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
  gravity (9.8) through Elden Ring's collision (`cs_phys_world_cast_ray`, one ray per 1/60 s through ray_filter and Snake's
  ground filter, the nearer hit -- `Puppet::CastWorld`, review 2026-10-31: ray_filter alone passed through the character-only
  floors he stands on and a grenade dropped on Stormveil's scaffold went off on the ground below; at a hit two parallel rays give
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
  - `kGEvTimeSkip` (v13): Elden Ring's clock moves by a minutes, spread over `[TimeSkip] smooth_ms` (`er/TimeSkip.cpp`, above).
- **Out:**
  - `kHEvHurt`: HP loss on the puppet, sent as the share of the Tarnished's max HP it took, in units of Snake's life / guest life scale (`[Combat] hurt_share`, docs/COMBAT.md; the hit hook measures the hit at full HP). `v0` is the unit direction from Snake toward the attacker (`last_hit_by`), in MGSV world. The kind is melee if the attacker is under 4 m away, projectile if further, fall after a hard landing, and other when the attacker is unknown.
  - `kHEvRespawn`: dead → alive.
  - `kHEvRested` (v10): `a = 0` a rest at a grace, `a = 1` a respawn (with `kHEvRespawn`): the guest refills Snake's ammo
    (Control hand-over, "Resting").
  - `kHEvTarnished` (v10): `a = 1` the player took the Tarnished (Tarnished mode), `a = 0` Snake is back.
  - `kHEvClimb` (v15): Elden Ring climbs a ladder with the hidden Tarnished (`a` 1 started / again on a turn or a new pace, 0 ended;
    `b` 0 up / 1 down; `v0[0]` m/s): Control hand-over, "Ladders".
  - `kHEvMantle` (v15): Snake climbs onto (`b = 0`) / vaults over (`b = 1`) a ledge `a` cm high, the move lasting `v0[0]` s:
    "Climb up and vault".
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
  Flasks are drunk while Snake plays with View + X (Control hand-over, "Flasks and the quick item"); their heal reaches Snake this way.

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
call setpos x y z | setyaw rad | ray x y z dx dy dz [filter] | kill yes
call rayall x y z dx dy dz [filter] [layers]   every hit along the ray: point, normal, body, its layer, hknp and Elden Ring
                             material (what the game's collector drops) -- Snake's own ray query (docs "Collision filters")
call raykeep x y z dx dy dz [filter] [keep] [layers]   the closest hit with dropped material classes / broad-phase layers kept
puppet set <key> <value>     live: ground_keep ground_layers ground_filter wall_filter wall_filter_also ground_probes step_up
                             snap_down radius jump_apex jump_air_step jump_air_control (not saved)
flag [name [on|off]]         render dead nodead nodamage nohit noattack nomove nogravity
puppet auto|off   menu auto|open|closed   debug on|off
tarnished [on|off|toggle]    Tarnished mode, as View + Start held 1 s toggles it
die [snake|er]               a test death: Snake's as MGSV reports it (kGEvDied, the default), or Elden Ring killing the Tarnished
ladder early on|off          kHEvClimb a=0 and the stick's latch at Elden Ring's EndTop/EndBottom (on, 0.6.0) or after the settle
mantle set <key> <m>         [Mantle] vault_max_height vault_max_depth vault_max_drop min_height max_height reach run_into_max live
flask [use|next|slots|on|off]   the quick item for the hidden Tarnished (View + X / View + D-pad down; [Flask]); slots lists all ten
noise [emit [r] [ahead] | mode sound|heard|off|ini | set <key> <v> | who [r]]   Snake's shots as Elden Ring AI sounds ([Noise])
mark handle <0x..>           mark one character (its capsule's height and the mark's tip logged)
puppet motion kinematic|root_motion   switch [Puppet] motion live (and zero the moved/asked, walls and breakers counters)
puppet hop [on|off]          dive = hop ([Puppet] hop) live; without an argument its settings
render [status | keys | set <key> <value> | probe]
hud [status | msg [info|good|warn|error] <text> | aim on|off|auto | set <key> <value> | card | bars none|all|auto|ini]
draw [skeleton|buddy|tracers on|off]
findq <expr> [max]           qwords equal to expr in writable private memory (class instances by vtable)
snap reset|take <label>|diff <a> <b> [max] [va vb]|add <addr> [bytes]|clear
                             live RE (er/MemDiff.h): bytes that follow a state you switch (exe .data, every FD4 singleton
                             and the objects it points to); full list in PhantomRingHost.snapdiff.txt
snap tree root <addr|player> | take <label> [depth [bytes]] | diff <a> <b> [max] [small]
                             the same over an object tree keyed by pointer path (survives reallocation across loads)
input key|keydown|keyup <vk> | mouse <dx> <dy> | wheel <notches> | mbutton left|right|middle down|up   SendInput inside
                             Elden Ring's Wine desktop (keydown/keyup: Elden Ring reads the keyboard once a frame; wheel and
                             mbutton are real mouse events: DirectInput sees them, unlike a VK_xBUTTON key event)
input virtualpad on|exclusive|shared|off   (on = exclusive since 0.6.0: every real XInput pad -- and a DirectInput game
                             controller -- is ignored meanwhile, for Elden Ring and MGSV; shared: only while no real pad
                             answers on user 0, the old `on`; [Keyboard] virtual_pad turns on a shared one)
                             test automation with no controller plugged in: user 0 reports a connected neutral pad to
                             Elden Ring's XInputGetCapabilities and polls, and Elden Ring is told devices changed
                             (WM_DEVICECHANGE): it enumerates and polls it, so `press` reaches Elden Ring itself (title,
                             menus, map, graces, prompts). Without it Elden Ring polls nothing and the bridge polls for
                             MGSV alone while a `press` lasts (`InputBridge::PollIfIdle`). 0.6.0: Elden Ring re-enumerates
                             its controllers only on WM_DEVICECHANGE DBT_DEVICEARRIVAL / REMOVECOMPLETE with a
                             DEV_BROADCAST_DEVICEINTERFACE of GUID_DEVINTERFACE_HID (2.7.2.0: its handler 0x141F6FB90 checks
                             wParam 0x8000/0x8004, type 5 and the GUID at 0x1430CF278, then flags its pad object (+0x899) for
                             the XInputGetState(0..3) loop at 0x141F2C570); the DBT_DEVNODES_CHANGED posted before did nothing
                             there (live 2026-10-31 with no controller: 16 polls at its start, none after `virtualpad on`).
                             Now one such arrival is sent (SendMessageTimeout from a thread of its own): `Elden Ring told a HID
                             game controller arrived (1 window(s) answered of 1)` -> it polled the virtual pad every frame
                             (70,619 calls in the session; menus, the map, graces, the ladder's Y, the flask all through `press`)
press <buttons|none> [ms] [lx ly [rx ry]]   the injected pad (lt/rt included), through the same masks as a real one
puppet hide render_flag|fade|none   switch [Puppet] hide_mode live
buddy                        the buddy, plus D-Walker's orders seen and the LB hints shown (er/BuddyOrders)
dog [status | on | off | all on|off | pick | test boss | style <s> | attack [style] [0x..] | release | bark | set <key> <v>]
                             D-Dog (er/Dog.h, "D-Dog in Elden Ring"): pick = every character with why he is (not) the pick
throw [status | arc on|off|auto|ini | meshes on|off]   Snake's grenades we fly, the aiming arc, [Render] projectile_meshes live
mines [detonate <id>]        Snake's placed explosives
decoys [add [m] [plain|active|shock] | pop <id>]   Snake's decoys; add = a test decoy m ahead of the Tarnished (no MGSV)
combat shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> x y z [share]   a test shot of Snake's at an
                             Elden Ring point, as a kGEvFire from his muzzle would be (share: MGSV's v8 damage share)
combat strike x y z | aimat x y z | self <id> [off]   a CQC strike there; the MGSV yaw/pitch/camera phase that aims at
                             it (for fake_mgsv `!cam`); the Tarnished's own SpEffect <id> on/off
combat set tranq_share|tranq_share_head|gas_share|choke_share|sleep_correction|boss_lives|boss_from|big_from|share_mult <v>
boom [status | lethal|stun|sleep|smoke|flame|breaker|shrapnel [radius] | mode ... | restore | pool]   looks 5 m ahead;
                             `pool`: Elden Ring's bullet/emitter counts and ours alive
boom look <kind> <bullet> [sfx] | set <key> <value> | raw <bullet> x y z   change a look live, tune the breakers (set
                             muzzle_flash on|off, muzzle_life <s>, muzzle_ahead <m>: [Combat]), fly
                             an unmodified Elden Ring bullet from the Tarnished's chest (what does Elden Ring break?)
stealth [hide on|off | speffect <id>]   the cardboard box (er/Stealth.h)
geom [learn [use] | offset <hex> | near|all [radius [filter|-filter]] | ladders [radius] | dump <ins>]   map assets (er/GeomProbe.h)
progression [off|level|weapon|both]   the Tarnished's level and right-hand armament, Snake's damage factor (docs/COMBAT.md)
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
| **C** `gbuffer` (default) | Elden Ring's own G-buffer, right after Elden Ring clears its scene depth, before its own G-buffer draws (`gbuffer_point`, below); its velocity target after its velocity pass; its shadow atlas | **Elden Ring**: sun + shadow mask, per-area light sets, sky/GI ambient, local lights (graces, torches), SSAO, fog, tone mapping, bloom, exposure, TAA. Snake casts the sun's shadow. |
| B `post` | Elden Ring's tone-mapped RGBA8 scene colour, right before the composite (the HUD stays on top) | ours: one clock-driven directional light + hemisphere ambient (`light_*`, `ambient_*`, `day_night`) |
| A `present` | the back buffer at Present (over everything) | ours, as B |

Stage C falls back to stage B for any frame whose structure doesn't match what it was built for (no G-buffer
bind with exactly these formats, unknown states, a different depth format...): `render: stage C not possible
(<why>)` once per change of reason, `stage C fallbacks` in `pr er render`.

| File | What it does |
|---|---|
| `D3D12Hooks` | Creates a throwaway DXGI factory, D3D12 device, direct queue and swap chain on a hidden window, reads their vtables, then detours `Present`/`Present1`, `ResizeBuffers(1)`, `SetColorSpace1`, `CreateSwapChain(ForHwnd)` and `ExecuteCommandLists`. It finds the presenting queue from swap-chain creation, `ResizeBuffers1`, `GetDevice`, or a direct queue whose pointer sits in the swap-chain object. Hands its dummy device to `RenderPso`. |
| `DebugLayer` | `[Render] debug_layer` (off by default, UNVERIFIED live; for validation runs on native Windows D3D12, docs/WINDOWS.md "For testers"): enables the D3D12 debug layer, GPU-based validation and DRED first thing in the init thread (refused once Elden Ring has a window: its device may exist, and enabling the layer then removes it), attaches to Elden Ring's device when the renderer finds it, logs validation messages (`ID3D12InfoQueue1` callback or polled at Present; 3 per id, 400 in all; tagged when raised inside our Present work) and, on a removed device, the reason plus DRED's unfinished lists and page fault. |
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

1. **After the list that clears Elden Ring's scene depth** (stage C, `[Render] gbuffer_point = clear`, since 2026-10-14;
   e.g. a 25-list batch #5: shadows in lists 2-6, G-buffer clears in 9, the depth clear and one depth-only draw in 10,
   G-buffer draws 11-17, a depth copy in 18, lighting from 19 on): the view constant copies (from the batch's G-buffer
   lists' bindings, recorded but not yet run), Snake into every shadow view, then Snake into the G-buffer (5 MRTs +
   scene depth with stencil ref `gbuffer_stencil`), states restored after. Elden Ring's own G-buffer draws then come
   after him and depth-test against him like against each other. A `render gbuf peek` is copied after the last
   G-buffer list (a fourth submission, only while one is pending), so it shows Elden Ring's characters too.
   **Why not after the last G-buffer list any more** (the point until 2026-10-14; `gbuffer_point = last`): Snake and
   D-Walker showed the scene behind them -- the player's session: wall arches through Snake's back at a grace, haze and
   rubble through his legs and pack. Live (replay of the real MGSV, the player's Stormveil spot, a brazier behind
   D-Walker at night): the brazier's fire, bowl and pillar on D-Walker's body, his body otherwise near black; `render
   set gbuffer_point k` (after the k-th G-buffer list) bisected it -- after list 10 or 13 (k = 2, 5) solid and properly
   lit, after 14-17 (k = 6-8, `last`) see-through; after 9 (k = 1, before the depth clear) gone. Not TAA (`velocity off`
   changed nothing), not the stencil class (0x10/0x90 the same; 0 unlit), not shadows; stage B (`stage post`) solid.
   Every one of Elden Ring's G-buffer pipelines writes RT0-2 (RT3/RT4 write mask 0), depth and stencil only -- all of
   which ours overwrite -- so whatever its draws leave per pixel for the lighting is elsewhere (UNVERIFIED what, e.g. a
   UAV); with Snake drawn first, the hidden scene behind him is never drawn where he is. A batch with no full clear of
   the scene depth falls back to the last G-buffer list (`render`: `G-buffer point: clear (N frames after Elden Ring's
   depth clear, M after its last G-buffer list)`, a warning the first time).
2. **After the velocity list** (stage C; forward batch #8, list 11: a full-screen camera-velocity draw, then
   ER's moving objects): Snake's motion vectors, depth-tested (read-only) against his own depth.
3. **Before the composite list** (stage B, or stage C's fallback): Snake into the RGBA8 scene colour.

`render probe` also lists texture copies and resolves per pass (`(copy src->dst)`, `(region ...)`), and `render probe
shaders` each pipeline's depth function / write (`z7w`: GREATER_EQUAL, writes), stencil write mask, pass op and function,
and its render-target write masks (`mfff00`).

**Verified live 2026-10-14** (ER 1.17.1 4K, `pr replay` of the MGSV agent's new real-MGSV recordings; before = `render set
gbuffer_point last`, after = the default `clear`, the same spot and moment): the player's Stormveil spot at 02:48 and 04:41
(night: the brazier through D-Walker gone, D-Walker's camo and Snake's launcher lit by the night ambient) and at 12:00 (the
bowl's fire through D-Walker gone); the grace next to it at 22:00 and 12:00 (the wall's stones and the pillar's torchlight
on Snake's back and pack gone; at noon he is lit, not near black); the crouched orbit recording all round his back and
pack: solid from every side. Lighting parity (the Tarnished shown next to him: `render set debug_offset 1.2,0,0`, `render
set show_tarnished on`, `flag render on`, ChrIns+0x24C poked to 1): at noon and at 23:00 both lit by the same light, the
Tarnished brighter only where his steel is (mean screen luminance at night: Tarnished 60, Snake 31 of 255 with light set
0, 54 / 33 with light set 1, the Tarnished's own here: `render gbuf peek` on him: stencil 0x50, light set 1); light set 3
(the earlier "character" set) leaves Snake black here even with the Tarnished on screen. `gbuffer_light_set = 0` stays.
**Re-verified with the real MGSV** (combined run 2026-10-14, both games on the default mapping; `render set gbuffer_point last`
/ `clear` switched live a second apart): the player's spot at 03:00 (the brazier and its pillar through D-Walker with `last`,
D-Walker and Snake solid and lit with `clear`), the same at noon, the Stormveil Main Gate grace at noon and at 23:00 (the gate's
lattice through Snake's back and legs with `last`, none with `clear`), and the player's haze view (fog, rubble and the brazier
no longer through his pack and legs). The Tarnished in Tarnished mode at the same spots and times is lit by the same light.
0 stage C fallbacks, view constants matched on every check.

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

**Material roles** (`er/MaterialRoles.*`, `[Render] role_guard`, `poster_check`; 2026-10-14; live pass A: 0 textures in a wrong
role, no false colours on D-Walker/D-Dog close up after buddy swaps; the poster box not re-checked). The player saw
D-Dog's coat in orange/green, D-Walker right next to the camera in magenta/orange and the cardboard box black. Offline, all
seven real-MGSV recordings decode cleanly (`dog_follow`, `idle_walker`, `run`, `rocket_back`, `no_buddy`, `fulton`,
`proxy_fulton`: every texture's mip chain consistent, every draw's texture keys constant over the recording, t0/t1/t2 never
shared between roles): D-Dog's materials are a white/grey base (`46f51ad4` fur cards: BC3 strands with alpha, t4 the coat's
colour atlas; `a7c7c5e2` body, `f249704d` head), their SRMs are yellow-green, D-Walker's orange with magenta, and both carry a
blue/pink/cyan layer noise in an extra slot. The false colours are exactly those maps drawn as a diffuse. Cause (guest,
docs/GUEST.md 15.3): textures were keyed by MGSV's texture pointer forever while our copy of the texture (and our reference)
was let go after a minute, so a texture MGSV made later at a freed address -- a buddy deployed later, a higher-resolution
texture streamed in as the camera nears, the box put on 48 minutes into session 6 -- went out under another texture's key.
Fixed in the guest. The host has two checks of its own, each `off | log | on`; both default to `log` (nothing changes on
screen, each texture the check would act on is logged once and counted in `render`), since neither is purely additive and
neither has been tried live:
- `role_guard`: diffuse, normal map and SRM keep apart by key (the first one a key is drawn as wins); extras t3-t5 neither
  are refused nor bar anything (MGSV's flat 128x128 normal map is also one shader's t5 in `fulton.prrec`, and the guest keys
  by content, so a generic texture may be a diffuse here and an extra there: until the review of 2026-10-14 one extra
  binding barred a diffuse from the next frame on). A diffuse whose own blocks are DXT5nm is refused too: red and blue ~0.5
  in >= 90% of the blocks of its <= 64-texel mip, and green both well below (>= 2% of the blocks) and well above (>= 2%)
  them. Recordings, every BC1-BC3 texture: every detailed normal map 100% red/blue with green below on >= 18% and above on
  >= 5%; every diffuse and SRM <= 14% red/blue; a mid-grey, sage or muted purple diffuse fails the green test; MGSV's two
  flat normal maps fail it too (drawn as a diffuse they are plain grey anyway); two normal-like extras pass. With `on`, a
  refused slot is left empty (grey diffuse / vertex normal / default SRM): plain instead of wrong colours, but an alpha card
  (hair, fur strands, lashes) whose diffuse is refused draws as an opaque grey quad, since the shader takes the alpha from
  t0. `render: texture ... (would be) left out (a stale guest key?)`, once per texture (32 at most); `render` shows
  `material guard (...): N textures in a wrong role (M normal maps as a diffuse; B bindings)`. Recordings: none expected.
- `poster_check`: with `on`, `poster_materials` paint the poster only where texture 4 is a poster by its content (>= 15%
  of its blocks the plain background (243, 242, 243); no texture in the recordings above 3%, D-Dog's near-white fur strands
  35% but those are a diffuse), since a box of the same shader without a poster may carry a layer mask there, which "replace
  the cardboard wherever t4 isn't near-white" paints black (a hypothesis from the shader: no box recording). The poster as
  drawn now is VERIFIED live while 15% was never measured on it, hence `log`: every box's texture 4 is logged once with its
  share (`render: material ... texture 4 (...) has N% of its blocks the poster background ...`), which is the measurement
  to set the threshold from before `on`.
`render set role_guard on|log|off`, `render set poster_check on|log|off` switch live.

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
transparency; in 2x2-pixel cells since 0.7.0, `near_fade_cell`: "Magenta / green tints" below): Snake per pixel (`near_fade = 0.25, 0.6` m from the camera), everything else per pixel
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

#### Night vision (`er/RenderNvg.cpp`, `[Render] nvg_*`, protocol v10 `kGuestNvg`)

MGSV's goggles only tint its own hidden picture (and darkened the HUD layer it hands over; the guest now leaves its full-screen
NVG overlay out of that layer, docs/GUEST.md). While GuestState `kGuestNvg` is set and Snake plays (not in a cutscene, a
load or death), Elden Ring's own picture becomes MGSV's night vision: at the composite point stage B and the world markers
use -- after Snake (stage C: he is in the scene already; stage B: drawn just before) and the throw arc, before Elden Ring's
HUD -- the tone-mapped RGBA8 scene colour is copied (`CopyResource` into a texture of ours, kept in COPY_DEST) and drawn back
through one full-screen triangle: linear luminance (the texels are sRGB-encoded, decoded) to the power `nvg_curve` (0.6:
the dark lifted more than the light, like a tube's automatic gain) x `nvg_gain` (5) with a soft roll-off `1 - exp(-gain
L^curve)` (highlights never clip hard), `nvg_floor` (0.025) black lift, coloured
`nvg_tint` (0.30, 1.0, 0.40 linear), per-pixel grain changing every frame (`nvg_grain` 0.07, strongest in the dark),
`nvg_vignette` (0.35) and optional `nvg_scanlines` (0); faded in and out over `nvg_fade_ms` (250) like the goggles. The
renderer's own root signature: the copy in the t6 table, the parameters in the 16 root constants of b1. Any failure (copy
texture, shader, pipeline) turns it off for the session, logged; frames with no composite point (stage A) get none. `render
set nvg|nvg_gain|nvg_floor|nvg_tint r,g,b|nvg_grain|nvg_vignette|nvg_scanlines|nvg_fade_ms <v>` live; `render` shows `night
vision: on (N frames drawn, copy WxH)`.

VERIFIED live 2026-10-13 (fake MGSV `!nvg on`, 4K): `render: night vision: copy of Elden Ring's scene colour 3840x2160
R8G8B8A8_UNORM` -> `night vision drawn over Elden Ring's scene before its HUD`; screenshots in the Castleward Tunnel (its dark
corridor turned bright green, the fake Snake and walker in it, the grace's prompt and compass untouched) and in Margit's
arena at 23:12 (`time+0x2C/0x30/0x28` pokes) with and without: the night scene green and readable, Margit, Snake, the trees
and sky in it, Elden Ring's HP, boss bar and quick items untinted on top. Cost: one 4K copy and one full-screen pass.
**With the real guest's HUD layer** (`pr replay` of the MGSV agent's recordings of real MGSV with the goggles toggled, Castleward
Tunnel): `nvg2.prrec` (the guest leaves MGSV's goggle overlay out of the layer) -- Snake (his goggles on his head) and the
tunnel in green, MGSV's NVG item box and weapon box clean on top, no darkening; off again 7 s later, the same frame dark as
before. `nvg.prrec` (the first take, the overlay still in the layer, 100% of it covered) shows what the fix removed: noise
and a dark veil over the whole picture, our night vision included.
**With the real MGSV** (2026-10-13, combined run): the goggles picked in MGSV's item wheel (D-pad left held, right stick down)
go on as they are picked, a short D-pad-left tap (<= 120 ms; MGSV takes a longer press for the start of a hold) takes them off
and on: `render: night vision on/off (MGSV's goggles on/off)`, they go off with a death and on again at the respawn. Elden
Ring's menu (Esc) opened over them stays untinted. The first curve (linear x 7) left the Castleward Tunnel nearly black (Snake
a dark silhouette); `nvg_curve` 0.6 x 5: the tunnel's mean brightness 8 -> 81 (of 255) with the goggles, Margit's arena at
20:00 41 -> 131, the sky not washed out (the pair of screenshots each). Nothing gets darker: MGSV's own overlay is left out
of the HUD layer (the guest), the layer showed only the weapon and NVG boxes.

#### Grass over Snake (roadmap 5.1; 2026-10-31, Elden Ring 1.17.2, the Church of Elleh's tall grass and bushes)

The known issue -- Elden Ring's grass drawn over Snake even where it stands behind him -- is the see-through of the old G-buffer
point, gone with `gbuffer_point = clear` (the default since 2026-10-14) but never re-checked in grass:
- **Pass order and depth** (`render probe shaders`): the frame's G-buffer batch clears the scene depth (list 10, `k9`), then draws
  lists 11-27: a depth/stencil pre-pass (`0f3269f0cbaacee5`, `z7w m00000`) and its colour pass with an EQUAL test (`ac8e8663099e0d63`
  `z3r`) for characters, foliage `f9ceb4dfb05c4045` (`st wffp3f8 z7w mfff00`: GREATER_EQUAL with depth write, stencil REPLACE 0xD2,
  translucency 0.4 in RT1.z) and the rest, all GREATER_EQUAL against the scene depth. Snake goes in right after the clear, so
  every one of them, grass included, depth-tests against him; the forward passes after the lighting test the same depth.
- **Per texel** (`render gbuf peek` now also copies the depth and stencil right after Snake's own draw and compares them with
  the final ones: `right after Snake's draw: ...; N of 256 texels Snake's then, M of them drawn over later (0xd2xM), K by
  something FARTHER than Snake`): 16x16 boxes every 48-64 px over the real Snake (`idle_walker.prrec`) standing in tall grass, at
  dusk and at noon: 32776 and 19067 of his texels, 2042 and 1341 drawn over later -- all by foliage (stencil 0xD2) **nearer** than
  him -- and **0 by anything farther**. Kalé's hat behind his shoulder: behind him, as it should.
- **Before / after frames** (`render set gbuffer_point last` / `clear` a second apart, the same spot and moment, noon and dusk):
  with `last` the bushes behind him show through his back, pack and legs; with `clear` he is solid and only the grass in front
  covers his lower legs.
- **One remaining way in, closed**: a batch whose depth clear came in an earlier batch of the frame used to fall back to the
  last G-buffer list (the see-through) for those frames. Now, when the scene depth was fully cleared earlier this frame
  (`RenderDepth::FullyClearedThisFrame`) and this is the frame's first batch with G-buffer lists, Snake goes in before its first
  G-buffer list (`render`: `G-buffer point: clear (N frames after Elden Ring's depth clear, M before its first G-buffer list
  (the clear in an earlier batch), K after its last G-buffer list: no clear seen)`). Never seen on this machine (0 such frames;
  0 fallbacks in every session since 2026-10-14); the player's grass reports may have come from such frames, or from before then.
- Not re-checked: the cardboard box in grass (no recording with the box; its draws go the same way as Snake's).

#### Minimum light on Snake (roadmap 5.3; `[Render] min_ambient` 0.008 since 0.7.0, `render set min_ambient <v>`; 2026-10-31)

Snake is lit by Elden Ring's world light set (0); the character sets that give the Tarnished his rim light light nothing while no
character of Elden Ring's uses them on screen, so in a dark tower at night Snake went nearly black. RT4 (R16G16B16A16_FLOAT) is
Elden Ring's emissive term: only its glowing materials write it (`mfffff`), every other opaque pipeline leaves it alone, and the
lighting adds it unlit. Snake's albedo x `min_ambient` goes there as a floor of light -- **not** at the injection: written there
it stayed under whatever Elden Ring drew in front of him later (their pipelines leave RT4 as it is) and his legs glowed through
the rock he stood behind (live). It is its own pass after Elden Ring's last G-buffer list, before the lighting
(`Gbuffer::RecordEmissive`): Snake's draws again, RT4 written alone, the depth read-only and tested as the velocity pass does, so
only where he is still the nearest surface (live: nothing through the rock in front of him, nothing on the Tarnished behind him).
Values (Gatefront at 00:11, the humanoid next to the Tarnished, `debug_offset`): mean luminance over his body 20.3 at 0, 22.4 at
0.002, 28.5 at 0.005, 30.1 at 0.01; 0.025 and up glow. 0.004 was the default: barely there under the moon, a visible figure
where nothing else lights him. By day the sun's light dwarfs it.

**0.7.0: 0.008** (2026-11-03, Stormveil's torch-lit tower room at 03:00, the real MGSV's Snake from a recording beside the
Tarnished, frames side by side at 0 / 0.004 / 0.006 / 0.008 / 0.01 / 0.02): 0 a black silhouette, 0.004 still dark next to the
Tarnished (whom Elden Ring's character light sets light), 0.008 his outline and gear readable beside the Tarnished, 0.01
brighter, 0.02 flat and glowing. By day it still disappears under the sun. With both games (0.7.0 integration, 0.008): Snake by
the torch at the foot of Stormveil's dark tower ladder and in the dim Rampart Tower room, his face, gear and launcher readable
(frames).

#### Magenta / green tints with the camera pushed into Snake (`[Render] near_fade_cell` 2; 0.7.0)

`render set near_fade_cell 1|2|4` tries it live; `gFade.x` in the shaders. The near-camera screen door dropped single pixels, and Elden Ring rebuilds each pixel's colour
partly from its neighbours': a 1-pixel door mixed Snake's pixels with the background's and his see-through ghost showed false
colours that changed with the background -- orange and blue, magenta and green, the two chroma axes (as a colour packed with its
chroma shared between neighbouring pixels would give; inferred from the frames, not from Elden Ring's shaders). The door now drops
2x2 cells, so each of Snake's pixels has one of his own beside it. Live 2026-11-03 (the real MGSV's Snake from a recording, the
camera pushed into him against a wall, `near_fade_cell 1` vs `2` side by side): 1 -- the ghost's body orange and its launcher
blue; 2 -- the ghost in his own olive and black; the door's dots show a little more. 4 (tried on the test model) is coarse. With
both games (0.7.0 integration, the Rampart Tower room, Snake walked into a wall and the camera turned against it): the see-through
near the camera (D-Walker's panel and Snake's launcher) in their own colours, 0 magenta and 6 green-ish pixels (channel lead > 60) of
864,000 in the near half of the frame.

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

**Elden Ring's bars while Snake plays** (0.7.0; `er/ErBars.*`, `[Overlay] er_bars` = auto | none | all; console `hud bars
none|all|auto|ini`). Snake's life, ammo and items are MGSV's HUD; Elden Ring's FP and stamina bars were the hidden Tarnished's
and meant nothing, its HP bar only mirrored Snake's life. `none` (auto, the default): all three go, through CSFeMan's own HUD
switch (singleton `CSFeMan`, RTTI `CS::CSFeManImp`; +0x78 hud_state, fromsoftware-rs CSFeManHudState: 0 HP / FP / stamina hidden,
1 shown, 2 a popup menu's, 3 the default), written 0 by the camera task (group 107) only while it is 3, so Elden Ring's menus keep
theirs, and put back to 3 when Snake stops (Tarnished mode, a menu, a load, a death). `all`: as before. **FP and stamina alone
could not be hidden** (tried live with the HUD forced On): CSFeMan's front-end values (fp, fp max, stamina, stamina max,
+0x98..+0xB8) and the Auto HUD's per-element "shown" bytes (+0x4EB8: 1 HP, 2 FP, 3 stamina) written 0 every frame after
CSMenuMan's update (task groups 126 and 107) left both bars drawn full -- Elden Ring's bars take their values elsewhere -- so the
mode that kept the HP bar was dropped. Live 2026-11-03 (HUD forced On, frames side by side: `all` / `none`): `all` the three bars
under the portrait; `none` the portrait and all three gone; Elden Ring's map and menu still show its own HUD.
- **On a ladder** (review 0.7.0): Snake drawn on Elden Ring's ladder (`Puppet::ClimbVisual`, the puppet held) is still his screen,
  as for MGSV's HUD layer: the bars stay hidden through the climb (they came back next to MGSV's HUD for every ladder).
- **hud_state 0 also hides CSMenuMan UI state 7** (found in the 0.7.0 integration with both games): the HUD signal the interactions
  ("Interactions" above) and the "Auto" HUD detector follow. Live: every takeover logged `ER HUD hidden (UI state 7) -- nothing on
  screen explains it`, the second one `Elden Ring's HUD looks set to "Auto"` (the press-less screens went off for the session), and a
  rest at the Rampart Tower grace ended after its 3.5 s minimum with the grace menu still up -- Snake had the pad (B reloaded his
  pistol) and only Tarnished mode could leave the menu. Now `ErBars::HidesHud()` (we hid it, or put it back less than 300 ms ago)
  makes `HudHidden` ignore UI state 7, and after one of Elden Ring's screens (any frame Snake's screen isn't up) the bars come back
  only 1.5 s later, so the screen that follows one still shows its own hide (the 1 s chain). And an interaction whose HUD was hidden
  at the press but came up since (a rest shows it at the sit-down) lets it tell the end from then on (`ER interaction: its HUD came
  up N ms after the press`). Live after the fix: 0 `nothing on screen` lines in the session; Y on the rest prompt -> `ER interaction
  (ActionButtonParam 6101)`, the HUD hid 2.3 s later (the grace menu, frame: "Rampart Tower", Pass time ... Leave, no MGSV HUD), B
  -> `ER interaction over after 13882 ms (its HUD is back)`, Snake back (frame). Tarnished mode with the HUD set On: the three bars
  (frame); back to Snake: gone (frame).

**The controls card** (0.7.0; `[Overlay] controls_card` first | always | off, `card_seconds` 12, `card_button` dpad_up,
`card_scale`; `Hud::CardTimer`, `ControlsCardText`, tests in test_host_overlay.inc). The first time Snake takes control (once per
install: `PhantomRingHost.firstrun` next to the ini remembers it; delete it to see the card again) a panel at the top centre of the
screen lists his controls: the jump (`[Input]` jump button), Y climbs / vaults / ladders and Elden Ring's prompts, View + X the
flask, View + D-pad down the next item, View tapped the map, View held MGSV's iDroid, View + Start held 1 s Tarnished mode, LB held
the buddy's orders, View + `card_button` this card again (the button names come from the ini). It fades after `card_seconds` or at
any press (one made after every button was let go since it appeared: the takeover's own buttons or a View
still held don't close it). **A closes it and does nothing else** (0.7.0 integration: the A also crouched Snake): while the card is
open on Snake's screen (`InputBridge::SetCardUp`), an A pressed is counted (`CardCloses`) and kept out of MGSV's pad until let go;
the card's last line says "A: close". **Only where it can be seen** (review 0.7.0): a card shown while Elden Ring's or MGSV's menu, a
cutscene, a load or a death is up keeps its time and ignores the presses made meanwhile (`CardTimer::Update` a_visible), the first
takeover's card starts only on Snake's screen, and `PhantomRingHost.firstrun` is written once the card was drawn (a takeover at a
grace's menu used to use it up unseen). View + D-pad up (a View shift like the
flask's, `PadGate` help) or `pr er "hud card"` shows it again. Live 2026-11-03: `overlay: the controls
card (Snake's first takeover; it fades after 12 s or at a press)`, `overlay: first controls card (45 px, 1601x394) drawn`; View +
D-pad up through the virtual pad -> `overlay: the controls card (View + D-pad up; ...)`; A dismissed it (frames: the card's seven lines at the top
centre over the Stormveil courtyard, then none). Live with both games (0.7.0 integration): `overlay: the controls card (Snake's
first takeover ...)`, `the first controls card is on screen -- PhantomRingHost.firstrun written` 0.36 s later (frame: the seven
lines and "A: close" over Stormveil, MGSV's weapon box with the pistol's SUP gauge); View + D-pad up -> shown again; A -> `the
controls card closed with A (that A kept from MGSV until let go)`, Snake still `Crouching` (the guest's flags before and after; an
A without the card then stood him up).

Console: `hud` (model + draw counters), `hud set <key> <value>` (the `[Overlay]` keys), `hud aim on|off|auto`
(force `kGuestAiming` for a test), `hud msg`, `hud card` (the controls card now), `hud bars none|all|auto|ini`. Cost: inside the renderer's 0.05 ms/frame CPU average; one
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

**0.6.0** (`er/Revival.h` `AtStop`, `TestRevival`; `Puppet::DeathSeconds`; the death trace): seen once in 0.6.0 testing, 2.4 s
after Snake died the Tarnished knelt on screen (his death, HP 0) and Snake wasn't drawn. Two holes found and closed:
- Whatever stops the puppet while a death is under way -- Snake's kill already out (KillForGuest, the dead bit a frame later), or
  this character dying -- the Tarnished now stays hidden until the respawn (`puppet: stopped (<why>) during Snake's death -- the
  Tarnished stays hidden`). Before, only the reason "player dead" did: Tarnished mode, a step-aside, MGSV's heartbeat lost, a ladder
  in those frames showed him dying, with the puppet off and no death of Snake's recorded (nothing of Snake drawn).
- A death of the Tarnished while the player plays him (Tarnished mode) is his: he dies on screen and Snake is not drawn. Live
  2026-10-31 (`tarnished on`, `call kill yes`): the dying Tarnished *and* Snake's test model popped up over him at the anchor
  Tarnished mode had left (frames at 0.3-6 s); `DeathSeconds` is now -1 for such a death (only Snake's own deaths draw him).
  A possible match for the frame from 0.6.0 testing (its stray DualSense presses could have made the chord); not proven.
- **Review of 0.6.0** (`er/Revival.h` `StopReason`, `ChordRefusedWhileDying`, `TestRevival`): the puppet's own character dead
  (the dead bit, in a loaded world) is now tested before every other reason to stop it -- "guest lost", Tarnished mode, a
  step-aside, a ladder, "disabled" used to win in a death's first frames -- and `Stop()` treats the dead bit as a death whatever the
  reason. View + Start is ignored from Snake's kill until the respawn (`input: View+Start during a death -- ignored`): on, it
  stopped the puppet with the Tarnished on screen; off, it would hand Snake a dead Tarnished.
- **The death trace** (every death, at 0.3, 1, 2.4, 4, 6, 9, 14 s): `puppet: death trace 2.4 s (Snake's death): puppet off, the
  Tarnished hidden (render bit 0, base transparency 0.00; hidden by us yes), dead bit 1, HP 0/680, Snake drawn (2 draws), MGSV
  ready, killed for Snake's death`, and a warning if the Tarnished is visible in Snake's death.
- Live 2026-10-31 (fake MGSV humanoid, Stormveil and the Church of Elleh): frames at 0.3, 1, 2.4, 4, 6, 9 s after `!die` (Snake's
  death as the real guest reports it), `call kill yes` (Elden Ring's own kill) and a 34 m fall (`landed at 25.7 m/s -> fall damage
  100%`): Snake drawn, no Tarnished in any frame; and the trace (no frames) of three enemy kills by the Church of Elleh's fences
  (`hit hook ... 5 calls, 5 cancelled` -> `guest: Snake died`): every line "the Tarnished hidden ... Snake drawn" until the respawn
  load (`ER player respawned` 11.7-18.4 s after the death).
- **With both real games** (2026-11-01, the review fixes in; Church of Elleh at night): `pr er die` (Snake's death as MGSV reports
  it), a 36 m fall (`call setpos` 36 m up: `guest: Snake died` on the landing) and a Godrick soldier's torch and sword (the guest's
  own death from Elden Ring's hits, `hit hook ... cancelled`), plus three more enemy kills by the same patrol: frames at 0.3, 1,
  2.4, 4, 6, 9 s of the first three -- Snake lying where he died (the soldier standing over him, D-Walker beside), YOU DIED, never the
  Tarnished; every death trace `the Tarnished hidden ... hidden by us yes ... Snake drawn (46-65 draws)` to 9 s, `another character`
  at 15.3 s (the respawn load), `ER player respawned` 13.1-18.4 s after the death, `puppet ON`, and Snake back at the grace (frame).
  A Tarnished-mode death (`tarnished on`, `die er`) the same night: the Tarnished dying on screen, no Snake drawn (frames 0.3-9 s). A
  View + Start held during it counted no chord at all, so the refusal above is unit-tested only. After a respawn load the new player
  object can be fully visible (`the Tarnished VISIBLE ... base transparency 0.92 ... another character` at 14.0 s) while Elden Ring's
  loading screen is still up; the puppet hides him in the frame it closes (no frame showed him).
- **The respawn's Tarnished hidden from his first frame** (0.7.0; `[Puppet] respawn_hide`, `respawn_hide_max_ms` 15000;
  `Puppet::RespawnHide` in Decide and AfterPhysics): the line above was not the whole story -- frames after the 0.6.0 review showed
  the new player object for up to ~4 s at the grace before the puppet's start. Now, through a death of Snake's, the player's ChrIns
  is compared with the one that died each frame: a new one is hidden at once (render bit cleared, its own base transparency
  kept; the originals saved so the puppet's `Start()` keeps them instead of saving hidden values) and stays so through the
  respawn until Snake takes over on it. Tarnished mode, MGSV gone or no takeover within 15 s of the respawn show him again; the
  death trace's `another character` lines say whether we hid him. Live 2026-11-03 (fake MGSV, two deaths of Snake's):
  `puppet: the respawn's new Tarnished hidden from his first frame (14.1 s after Snake's death; render bit 1, base transparency
  1.00 kept) -- Snake takes over after the respawn`, `ER player respawned (17.3 s after the death, after the load)`, `puppet: the
  respawn's Tarnished stayed hidden 3.2 s, from his first frame to Snake's start` (the second: 14.2 s, 3.0 s); a frame after
  the takeover: Snake's model at the grace, no Tarnished (the hidden window itself is from the log). With both games (0.7.0
  integration, the real MGSV): five deaths of Snake's (a beast, Stormveil's soldiers twice, a 21 m fall, the courtyard group) --
  each `the respawn's new Tarnished hidden from his first frame (14.1-14.2 s after Snake's death ...)`, `ER player respawned (11.3-17.3
  s after ...)`, `stayed hidden 3.1-3.2 s, from his first frame to Snake's start`; the death traces `the Tarnished hidden ... Snake
  drawn` at 0.3-9 s; a frame after the respawn (the Rampart Tower room): Snake and D-Walker, no Tarnished.

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

Roadmap 2.14 tier 1, `[Keyboard]` (opt-in, off by default; **live pass B 2026-10-15: works** with keys and mouse events sent into
Elden Ring's window by the console's `input` -- a player's own keyboard and mouse not yet tried). `er/KbmMap.h` and `er/KbmFilter.h` (pure,
`tests/test_host_kbm.inc`) and `er/Kbm.h` (the glue). While Snake plays, the keys and the mouse become the pad MGSV gets; everything the bridge does with a
pad (Start/View gate and chord, prompt button, MGSV's menu owning the pad, held-over buttons) works unchanged for them.

- **Keys**: `GetAsyncKeyState` once a frame (`Kbm::Tick`, FrameBegin after `SetPadMode`), only while Elden Ring's window is
  in front. Keys held when Snake takes the keyboard (an Elden Ring menu's confirm) are ignored until released.
- **Mouse**: Elden Ring's own DirectInput mouse. It reads keyboard and mouse through DirectInput8 and imports no raw input
  (binary-scout §2), so `IDirectInputDevice8::GetDeviceState` (slot 9) and `GetDeviceData` (slot 10) are hooked inside
  dinput8.dll (found through probe devices of both kinds, W and A interfaces; one hook per distinct function). The deltas
  are the device's counts, never the cursor position. No raw input is registered: under Wine dinput's mouse may itself use
  raw input, and a second registration in the process would take it over.
- **Mouse curve** (`KbmMap MouseStick`): the speed (counts/s, smoothed `mouse_smoothing_ms`) per frame; u = speed x
  `mouse_sensitivity` / 1000, u' = u (1 + `mouse_acceleration` u), deflection = `mouse_anti_deadzone` + (1 - it) min(1, u');
  below `mouse_min_speed` centred. Default 0.6 / 0.5 / 0.2: 1000 counts/s = 82% of the stick. Live tuning: `input kbm sens 0.8`.
- **What Elden Ring sees** (DirectInput filtered after the real read): while Snake plays nothing of the mouse and of the
  keyboard only `er_keys` (Esc; not even that while MGSV's menu is open: Esc is then Start for MGSV, to close it). In
  Tarnished mode only the chord key (G) is kept from it: held, it is Start + View on the pad (the gate's chord); let go before
  `[Input] chord_ms` it reaches Elden Ring as a 120 ms tap at the release (G = its map; counted for MenuWatch's latch).
  During an interaction's buttons-only phase (`InputBridge::SetButtonsOnly`: a pickup, a lever, a dialog before its HUD
  hides) it gets `er_keys` + `er_interaction_keys` (E, Q, Enter, arrows) and no mouse: the keyboard's share of the pad's
  `kInteractionButtons`, so WASD, Space, F or a click can't walk, roll, jump or swing the hidden Tarnished. Elden Ring's
  menus and everything without Snake: untouched. MenuWatch counts a menu key only when it reaches Elden Ring
  (`Kbm::KeyReachesEr`), so G held for the chord is not taken for its map.
- **Held keys** (`KbmFilter`, review 2026-10-14): Elden Ring never sees a press it shouldn't act on and never misses the
  release of one it saw. Snapshots (`GetDeviceState`) are filtered by zeroing, a release: a key hidden while held stays
  hidden until let go whatever changes under it (Esc that closed MGSV's menu doesn't open Elden Ring's once `kGuestInMenu`
  clears; E that started an interaction doesn't start a second; G held into a menu isn't its map). Buffered events
  (`GetDeviceData`) are filtered by dropping presses only: a release passes whenever Elden Ring had the press, and a key it
  has down that is hidden now gets a made-up release (else W held in Tarnished mode would stay down in Elden Ring after
  the chord, the Tarnished walking on alone). Mouse buttons alike. The DIK tables are rebuilt (`input kbm on|off`) into a
  spare copy and published by one index store, never half-built under Elden Ring's input thread.
- **Device**: `Kbm::Apply` (XInputGetState detour, before publish and masking) replaces the published pad with the keyboard's
  when the keyboard is the device in use: the last one used, the real pad on a tie (pad activity = past XInput's dead zones).
  The pad is where it starts, so controller players never notice.
- **No controller**: Elden Ring then never calls XInputGetState (until told a HID device arrived: the virtual pad does that since
  0.6.0, see `input virtualpad` in the console list). `virtual_pad = auto` turns on the bridge's (shared) virtual pad
  (`input virtualpad on`) the first time the keyboard is used while Elden Ring hasn't polled XInput for 0.5 s.
- **Glyphs**: `keyboard_glyphs` makes KeyGuide set Elden Ring's keyboard glyphs ("E: Rest") while the keyboard plays Snake.
- **Cursor**: `capture_cursor` clips it to Elden Ring's client area while the keyboard plays Snake (only our own clip is undone).
- Console: `input kbm` (state, device, DirectInput read counts by kind and the last state size, packets written, taps),
  `input kbm on|off`, `input kbm sens|accel|antidz|smooth|minspeed <v>`.

- **Chord tap** (`chord_tap`, live pass B): in Tarnished mode a tap of the chord key used to reach Elden Ring as that key (G),
  which opens its map only where its map is bound to G -- the tested save had it on M (System > Keyboard/Mouse Settings > Key
  Bindings), and nothing happened. `view` (default) hands Elden Ring the pad's View for `kTapMs` instead (`Kbm::Apply`; the gate
  passes it on at its release), its map whatever its key bindings; `key` is the old way, `off` nothing.

Live pass B (2026-10-15, `input key/keydown/keyup`, `input mouse`, and new `input wheel <n>` / `input mbutton left|right|middle
down|up`, real mouse events DirectInput sees): Elden Ring reads both devices **by state** once a frame (keyboard 60 state reads/s,
0 buffered; mouse the same, last size 256); Ctrl tap stood Snake up, W walked 2.6 m in 1.5 s, Shift+S ran 5.9 m, 600 mouse counts
turned the camera 51 deg, RMB aimed and LMB fired 2 rounds, R reloaded, 1-4 and the wheel switched weapons, V held raised the
binoculars, Space dived (and hopped), the middle button jumped 1.19 m, Q + mouse + middle button gave D-Walker's Whistle, E at a
grace rested (keyboard glyph "E: Rest at site of grace"), Tab tapped / held opened the map / the iDroid, Esc closed the iDroid
without Elden Ring's menu (tap and 0.5 s hold) and a fresh Esc opened it, G as Snake opened nothing, G held 1.3 s switched
Tarnished mode both ways, W held across the chord in either direction left nobody walking (0.0 m / 0.04 m in 2 s). A controller was
plugged in, so `virtual_pad = auto` never had to act. Open: a real mouse's counts and the feel of `mouse_sensitivity`.

### Offline verification

- `tests/test_host.cpp` (wine) covers:
  - the console expression parser;
  - the anchor math (round trips, delta/heading consistency for every erForward/mirror combination);
  - the kinematic controller against a synthetic world (flat walk, wall stop at radius, slide, 0.3 m step, 1 m block, ledge fall + landing at ~9.9 m/s, no drift), `Enclosed()` and the unsticking (back to where it stood free, across an origin shift);
  - (`tests/test_host_motion.inc`) low ceilings, stalls, and the ground under the whole body (`TestGround`: planks with gaps, a hole, a thin platform, a boardwalk, falls onto them, the start over a gap and past an edge);
  - the link against a scratch mapping (attach/alive/foreign nonce, GuestState, events);
  - the console with no game (peek/poke of own memory; game symbols answer "unavailable").
- `tests/test_host_chord.inc` (included by test_host): Start/View and the Tarnished-mode chord (`PadGate`: taps, holds, the
  iDroid hold, the chord in either order, early release, mode changes under held buttons, MGSV's menu, `chord_ms` 0), and
  the fall watchdog against the synthetic world.
- `tests/test_host_kbm.inc` (included by test_host): keyboard and mouse as Snake's pad (`er/KbmMap.h`): key names both ways,
  the default bindings, walk/run/diagonals, every button and trigger, Esc as Start only in MGSV's menu, the wheel's queued
  taps, the mouse curve (anti-dead zone, monotonic, acceleration, smoothing, a still mouse centres, frame-length
  independence, stall clamp, invert), the device arbiter, the Tarnished-mode chord-key tap, `[Keyboard]` from an ini; what
  Elden Ring's DirectInput sees (`er/KbmFilter.h`): the DIK tables, each scope (the interaction's buttons-only phase), held
  keys across scope and MGSV-menu changes (Esc closing the iDroid), the chord key's tap, mouse buttons, buffered releases
  never lost (made-up ones for keys hidden while down), peeks on a copy.
- `tests/test_host_overlay.inc` (included by test_host): every overlay rule (status/boot/failed/absent
  lines, messages, HUD fade on menu/death/guest loss, reticle auto/on), the reticle picture and a GDI text
  panel under wine (`PHANTOM_DUMP_OVERLAY=<dir>` writes them as raw RGBA for a look).
- `tests/test_host_combat.cpp` (wine): the damage model (bigness, reference HP, MGSV's share, headshots, a field boss's
  10-20% per magazine), explosion looks, the breaker / flame / object-only attack rows (synthetic and on the real
  regulation: the flying Fire Pot's own attack is object-only, ours become so), `Stealth::BoxHidden`, `Rounds::FlightOf`.
- `tests/test_host_load.cpp` loads the DLL into a non-Elden Ring process. It must log `exe mismatch` and do nothing else.
- `tests/test_render.cpp` (wine): every renderer shader (VS per layout, `PSMain`, `PSGbuffer`, `PSVelocity`,
  `PSShadow`, the night vision's `VSNvg`/`PSNvg`) compiles with Wine's d3dcompiler and its `FrameCB`/`DrawCB` sizes (D3DReflect) equal the C++
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
5. Motion follows Snake: walls stop it, it slides, it climbs steps under 0.5 m, it falls off ledges. `ray_filter 0x2000058` and `ground_filter 0x2000016` must hit the world. Check with `call ray <pos> 0 -5 0` and `call ray <pos> 0 -5 0 0x2000016`.
6. The skeleton stands on the puppet (green, CSEzDraw), faces like Snake, and limbs are not mirrored. If not, adjust `[Axes] mirror_x`, `mgsv_yaw_sign` and `mgsv_root_offset_y`. If nothing draws, check `ez`.
7. Camera: ER renders from `pers_cam_1` written at group 107 (RE E3). If not, try `task_group = 144`. Also check the fov units (the log prints ER's own value) and the forward row sign. Walls pull the camera in.
8. Hits on the puppet produce `kHEvHurt` (debug log), and the HP bar mirrors Snake's life. Snake's death triggers YOU DIED, and the respawn sends `kHEvRespawn`.
9. Buddy: `buddy` shows D-Walker's type and flags. The cyan marker sits on the ground next to Snake. Riding raises the speed and step limits. Buddy shots are traced.
10. A guest heartbeat timeout (kill MGSV) hands control back the same frame: `puppet OFF (guest lost)`.
