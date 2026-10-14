# Status (2026-10-14, 0.3.0-alpha)

The detailed record of what has been tested live, and how. The short version is in the [README](../README.md).

Tested with both games running (`pr launch`, Limgrave: Gatefront, Agheel Lake, the Stake of Marika). The last pass
(2026-10-12 evening: ~2.5 hours with both games, 10 minutes of it continuous play) drove the controller through the
plugin's test pad, with no player; "evening" below marks what it re-tested.

![Snake and D-Walker at Gatefront, lit by Elden Ring, MGSV's HUD on top](screenshots/snake_walker_gatefront.jpg)
![Riding D-Walker into Gatefront Ruins](screenshots/riding_dwalker_gatefront.jpg)

## Combined run, 2026-10-14 afternoon (real MGSV + Elden Ring, both on the default mapping; 0.3.0-alpha)

Three launches (`pr launch --er-headless 3840x2160`, the virtual pad; one Elden Ring relaunch for a fix), ~2.5 hours, the
player's save: their spot in front of Stormveil's main gate (the Erdtree over the castle), the Stormveil Main Gate grace, the
Roundtable Hold and its basement stairs, Gatefront. Every item checked by frames and both logs; the save was put back afterwards.
The player's session-6 reports, item by item:
- **See-through Snake and D-Walker: fixed.** `render set gbuffer_point last` (the old point) / `clear` (the default) a second
  apart, same frame otherwise: at the player's spot at 03:00 and at noon the brazier and its pillar show through D-Walker and
  Snake is dark with `last`, both solid and lit with `clear`; at the grace at noon and 23:00 the gate's lattice shows through
  Snake's back and legs with `last`, never with `clear`; the player's haze view (fog, rubble, the brazier) is solid now. The
  Tarnished (Tarnished mode) at the same spots and times is lit by the same light.
- **Stuck under the Roundtable Hold's basement stairs: fixed.** The staircase found by a floor scan (HOST.md "Low ceilings");
  four lanes under it: standing, Snake stops where 1.85-2.01 m of room is left, crouched where 1.32-1.57 m is, standing up there
  and walking out works in every lane; corner pushes end in `stuck at ... back to ... where he stood free`. Found and fixed on the
  way: a crouched Snake crept under the treads through the seams between them (to a 0.57 m ceiling) and was then held for good
  by a lower stringer; a free one could take moves ending under a 1.6 m underside.
- **View, then Start: the chord.** Start 550 ms after View: MGSV's iDroid flashes open for ~1 s and closes by itself, Tarnished
  mode 1.0 s after Start (three times); Start then View: Snake back; View held alone: the iDroid, Start closes it.
- **Buddies from MGSV's menu.** Dismiss D-Walker, deploy D-Horse, D-Dog, D-Walker again, each with the iDroid through the pad
  (the host's corner: "D-Walker dismissed -- no buddy now", "Buddy: D-Dog is out"); D-Dog follows and marks Elden Ring's enemies.
- **Fulton of a sleeping Elden Ring soldier: works.** Gatefront: two darts, walk up, MGSV's Fulton prompt and "ZZZ" marker over
  him, Y held -> 5 s later Elden Ring's balloon lifts him away, gone, +64 runes (HOST.md "Fulton"). Elden Ring's sleep lasts ~50 s.
- **Full proxies (`!proxy on`, off by default): not ready.** With the real host MGSV went to alert phase 3 within 80 s and an MGSV
  riot-shield reinforcement shot Snake on the apron (GUEST.md 20.5); kept off, the guest now warns when MGSV's alert phase leaves 0.
- **How to play pass** (README, scripted with the pad): A crouch/stand, A held prone, run, X dive (5.7 m), camera, D-pad up/down
  (rifle, pistol), LT aim, RB first person while aiming, RT fires (ammo 8 -> 7, the dart into Elden Ring's world), B reload, D-pad
  right + LT (the arc) + RT throw, LB order (`the player's order 20 -- "D-Walker: Whistle"`), Start menu + B, View map + B, View
  held iDroid + Start, the chord both ways, NVG (green night vision), the cardboard box on and off (RT), Y mounts D-Walker, a
  17.5 m ride, Y held gets off, a rest at the grace (`kHEvRested 0`, grenades 7 -> 8), fast travel (six), two deaths at
  Gatefront and the respawns (`kHEvRespawn, kHEvRested 1`).
- **10 minutes of continuous play** at the Stormveil Main Gate grace (the player's spot; scripted, 602 s: 65 walks/runs, 49
  pans, 21 stance changes, 2 dives, 15 weapon switches, 12 reloads, 13 aimed shots (4 in first person), 7 grenades, 10 item
  uses (box/NVG), 7 LB orders, 10 Tarnished-mode round trips, 6 Elden Ring menus, 10 maps, 10 iDroids): Snake in control after
  every action, **0 warnings and 0 errors in both logs** -- the whole Elden Ring session (the stairs tests, the regression pass,
  the play) and the whole second MGSV session (2.5 hours of it).
- **Review fixes** (2026-10-14): damage_route speffect's guarded misses no longer confirm the ladder, collect-logs.cmd never zips
  dxdiag's raw report, the proxies are suspended while the frame skips them and refused without `[Boot] DisableSoldiers`.
- Found: D-Dog's coat sometimes in false colours (orange/green), D-Walker right next to the camera in magenta (MGSV's near-camera
  fade); the cardboard box drawn very dark; the stage C "no G-buffer bind seen yet" after every ResizeBuffers was a warning (20 in
  the player's session 6): now info, warned only if it lasts 120 frames.

## Combined run, 2026-10-13 morning (real MGSV + Elden Ring, both on the default mapping; release 0.2.0-alpha)

Two launches (`pr launch --er-headless 3840x2160`, the virtual pad; the second with `PHANTOMRING_BUDDY=horse`), ~50 minutes;
the player's save at the Castleward Tunnel grace, then Gatefront, Fort Haight and the Stormhill Shack. Logs and screenshots were
looked at for every item; the save was put back afterwards (sha256 equal to this session's first backup).

- **Versions**: both logs' first lines `PhantomRingHost v0.2.0-alpha (commit ..., protocol 11)` / `PhantomRing guest (dinput8.dll)
  v0.2.0-alpha (...)`; a stand-in guest naming protocol 12 / v0.3.0 -> Elden Ring's corner in red "PhantomRing versions differ:
  Elden Ring plugin v0.2.0-alpha, MGSV plugin v0.3.0 -- reinstall both"; one naming nothing -> "... MGSV plugin from before
  v0.2.0-alpha ..." after 15 s; both gone when it stopped. `pr doctor`: 19 ok, 1 warning (backups 23.7 GB), 0 failures.
- **Regeneration** (MGSV's post-hit curve, `[Combat] RegenGate`): an 18% fall held ~3 s, then MGSV's ramp, full 7.25 s after
  the hit (gate off: 2.0 s). Margit, Snake standing still: gate on, dead after 5 hits in 12 s (again ~17 s); gate off, 22 hits
  in 75 s and alive. Life may tick up 50-140 for a moment before the curve's first write (GUEST.md 18.15).
- **Ladders** with the real MGSV (Fort Haight's tower): up three times (+19.1 m, Snake in control at the top), down once
  ("Descend" when he faces the hole, -18.8 m). Walking on into the hole from the top dropped him down the shaft twice (92%).
- **D-Horse** (`PHANTOMRING_BUDDY=horse`): deployed and drawn beside Snake, whistled over by itself, mounted with the horse's
  values; on Stormhill's open slope 98.3 of 98.3 m asked in 8 s at a 15-16 m/s gallop; into the shack's ruins: blocked and one
  unstick; off a 13 m ledge: 55% fall damage; `!buddy walker` back to D-Walker in 1 s. 0 warnings in that session.
- **Progression** (live switch between shots at one soldier): off 35, level 31, weapon 35, both 31; back to off.
- **Root motion vs kinematic**, 4 minutes each on the Stormhill loop: kinematic 63% of legs / 73.3% of the distance, root motion
  64% / 65.5%, 0 warnings each: kinematic stays the default.
- **10 minutes of continuous play** (Stormhill Shack; scripted: 20 walks, 15 stance changes, 10 dives, 10 grenades, 10 NVG round
  trips, 10 D-Walker whistles, 9 weapon switches, 9 maps, 7 Elden Ring menus into Equipment/Inventory, 7 boxes, 6 iDroids, 6
  Tarnished-mode round trips, 3 bursts with reloads): 0 problems in the script's checks, **0 warnings and 0 errors in both logs**.
  A rest afterwards: `kHEvRested 0` -> `stocks ... 1->8` (Melina sat down instead of the grace menu, as before).
- **Found and fixed live**: in that play Snake walked into the shack and was stuck for good -- only the waist ray was blocked
  all round, and the boxed-in check looked at the knee and chest only; it now counts every probe height (seen working in the
  D-Horse session: `boxed in ... back to where it stood free`).
- Whole run: the host logged only the two deliberate version-mismatch errors; the guest one warning, during the gate-off A/B at
  Margit (a life write that did not stick).

## Combined run, 2026-10-13 (night; real MGSV + Elden Ring, the player's save at Margit)

Both games on the default mapping (`pr launch --er-headless 3840x2160`, the virtual pad), ~1 h, two launches plus an
MGSV-only relaunch; Castleward Tunnel and Margit's arena. Logs and screenshots were looked at for every item.

- **Tarnished mode** (View + Start held 1 s): 28 toggles, Elden Ring's menu never opened, MGSV's iDroid never opened, the guest
  parked and back each time (`tarnished mode: on -- Snake parked` / `off -- Snake has the pad again`); the Tarnished walked
  3.5 m and Snake took over there; mid-fight the Tarnished drank a flask (74% -> 100%) -> `Snake +2044 life (kHEvHeal 511)`.
- **Refill**: rest -> `kHEvRested 0` -> `magazines 36:17->30; stocks 197->210`; every respawn -> `kHEvRested 1` -> life 7801 and
  `36:4->30; 184->210`.
- **Fall watchdog**: 150 m above the grace -> `falling 3.5 s, 60 m with no landing -> Snake dies of the fall` -> respawn.
- **Margit**: "Traverse the mist" shows while Snake plays; Y -> `puppet steps aside for 4500 ms` -> through -> `puppet back`,
  Snake inside, his bar up and `menu closed` (its entries `HUD too`), three times. 26 rounds 12.7%, 30 rounds 14.4% of his
  4174 HP; his hits 11-45% of Snake's life (29 hits); deaths -> the grace -> fog again with full ammo. Never below 85%.
  The intro cutscene doesn't play again on this save: the skip fix is unit-tested only.
- **Night vision**: the goggles from MGSV's item wheel -> green night vision over the world and Snake, under Elden Ring's HUD
  and menu; with `nvg_curve` (new) the dark tunnel's mean brightness 8 -> 81 of 255, the night arena 41 -> 131.
- **Aim assist**: `boot: auto-aim correction: off (was on)`; 50 aim directions, no snap (<= 5.7 deg, parallax).
- **Esc**: `menu OPEN` / closed with Esc, MGSV's HUD layer hidden meanwhile.
- **Found and fixed live**: after the grace's "Pass time" Elden Ring showed the grace menu again 4.7 s later with Snake holding
  the pad (B reloaded, the menu could not be closed): sitting at a grace now keeps Elden Ring's screens Elden Ring's; the guest
  warned that a life write "did not stick" on Margit's hits that had (MGSV's regeneration): the check fixed (0 in 11 hits).
- **How to play pass**: A crouch/stand, A held prone, X dive (7.2 m), B reload, D-pad down/up/right (handgun, primary,
  grenade), RB first person while aiming, Start menu, View map (and fast travel: Snake back 5 s after the load), View held
  iDroid (Start closes it), LB order (`D-Walker: Whistle`), Y mounts D-Walker first try, riding, Y held dismounts, the box,
  a sleep grenade thrown at Margit, a CQC strike (standing; crouched RT does nothing), a rocket fired and exploding.
- **10 minutes of continuous play** (scripted: 24 walks/runs, 21 pans, 14 stance changes, 11 dives, 10 bursts with reloads,
  10 weapon switches, 20 grenades, 11 NVG toggles, 13 D-Walker whistles, 14 Tarnished-mode round trips, one rest): 0
  warnings and 0 errors in both logs (the guest's one false life-write warning came 7 minutes earlier, in the Margit fight).
- **Root motion** with the real MGSV: 100% of the asked distance on the same loop where kinematic gave 74%.
- **Found and fixed live, second part**: Start -> Inventory (or Equipment) gave A/B/D-pad to MGSV inside the inventory (D-pad
  right switched Snake's weapon, only Start got out), and one A acted twice (the inventory and an item's context menu; in
  Equipment it opened the right-hand list): Elden Ring's HUD hiding inside a menu now keeps the menu open and a submenu
  replacing its menu arms at once -- the equipment overview, the inventory grid, B back, B closed, MGSV's HUD hidden throughout.
  Tarnished mode now refuses to give Snake back on Torrent ("get off Torrent first"; the player's whistle at Stormhill).
- **10 more minutes with the final build** (Stormhill Shack, daylight; scripted: 22 walks, 17 stance changes, 12 dives, 11
  grenades, 10 box entries, 12 NVG round trips, 11 D-Walker whistles, 9 Tarnished-mode round trips, 8 Elden Ring menu round
  trips into Equipment or Inventory, 10 maps, 8 iDroids, a rest at the grace with its menu closed by B): every menu open
  while up and closed after (0 problems), the rest refilled the grenades (3 -> 8); 0 warnings and 0 errors in both logs
  (the guest's covering the whole second MGSV session, the second Margit fight included).

## Elden Ring side, 2026-10-13 early morning (fake MGSV and the real MGSV's D-Horse recording; details in HOST.md / COMBAT.md)

- **Ladders**: Snake's Y on "Climb"/"Descend" -> the Tarnished climbs (whole pad, Elden Ring's camera) -> Snake back where he
  gets off, up and down Fort Haight's 19 m tower ladder; the chord on a ladder is Tarnished mode, refused back to Snake there.
- **D-Horse**: the real ride replayed at Agheel Lake: mounted with the horse's values, 118 of 118 m of gallop, drawn on the horse.
- **Progression** (off by default): level 15 / Halberd +0 read and cross-checked; a body hit 35 -> 31 with `level`.
- **Root motion vs kinematic**, 10 minutes each around the Stormhill Shack: legs arrived 78% vs 58%, distance 75.5% vs 68.4%.

## What you can do now

- **Start it and wait.** MGSV boots hidden and unattended (2-4 min); the corner of Elden Ring's screen says
  "MGSV: loading ..." meanwhile and you can play the Tarnished as usual. Then "Snake is in control".
- **Be Snake.** Your controller moves MGSV's Snake (walk, run, crouch, crawl, dive); Elden Ring's character
  follows through Elden Ring's collision (Elden Ring's own movement: it pushes Snake out of carts and rocks instead of
  letting him walk into them, and catches up with him uphill); Elden Ring's camera is MGSV's. Snake and D-Walker are MGSV's own models
  in Elden Ring's frame, lit by Elden Ring (sun, shadows, night, torches, grace light, fog), with MGSV's HUD on top
  (weapon box, reticle, notifications, prompts).
- **Throw grenades.** Hand grenades, sleep gas, stun and smoke grenades, molotovs (D-pad right, LT aim, RT throw). While
  LT aims, MGSV's arc is drawn in Elden Ring's world: a line of bright dots and a ring where it lands, through Elden Ring's
  walls and slopes. The grenade flies, bounces and rolls through Elden Ring's world (drawn as a small ball) and goes off
  where it ends up, with Elden Ring's own pot effects: a fireball, a flash, a sleep mist, a smoke cloud. Sleep gas puts
  enemies to sleep in ~1.5-3 s (everyone in the cloud: live, three enemies, 68 build-ups, none lost), stun grenades
  stagger, neither kills. Claymores, mines and C4 (LT+RT places them) lie where you put them with a blinking light; an
  Elden Ring enemy walking into one sets it off (live: a sleep-gas mine 0.8 m from a Gatefront soldier, set off 1.5 s
  after it was placed, put him to sleep; C4 only from your detonator). Mines stay where they lie while you play on (evening:
  a sleep-gas mine kept through a puppet restart, then set off: Elden Ring's sleep mist where it lay, MGSV's own blast
  skipped 0.9 m from Snake and D-Walker); a death or a load clears them (evening: after a death MGSV set the old mine off
  out of the way and all 8 of its blasts were ignored). Throws were not re-tested in the evening (the test pad's LT+RT
  never made MGSV throw).
- **Elden Ring's compass** follows Snake's camera (it used to swirl with Elden Ring's own camera): live with both games,
  35 samples standing still all read 333.6 deg; turning, it moved 6.7 deg per 0.1 s sample, evenly (sd 0.55).
- **A steady camera.** MGSV, hidden, runs about 55-60 frames/s and Elden Ring 60: one Elden Ring frame in seven or eight
  used to show the last MGSV frame again and the next one jumped two. Every Elden Ring frame now shows one moment ~20 ms
  back, blended from the two MGSV frames around it (the camera, Snake's motion, his and D-Walker's poses): panning, the
  picture's jitter went from 5.2 to 0.6 px rms at 4K; Snake's running steps are smoothed too. MGSV itself runs closer to
  its 60 frames/s (its job system's 1 ms sleeps shortened), the camera no longer swings round when Snake runs toward it
  (the treadmill's Lua warp turned MGSV's camera) or rides D-Walker (it jumped 75-170 deg once a second; now at most
  0.23 deg per frame), comes back out from behind trees eased instead of popping, and eases over steps. Re-measured with
  both games: panning 0.15 px rms between Elden Ring's own hitches, running 0.4 px / 0.5 cm rms
  ([docs/HOST.md](HOST.md) "Frame pacing"). Evening: running 0.19 px rms, panning p95 1.4 px, but an 8 s pan had
  three one-frame pops (up to 51 px; one a 2 m camera pull-in by collision) that lift its rms to 3.2 px.
- **Fight Elden Ring's enemies with MGSV's weapons.** Aim (LT), fire (RT), switch weapons (D-pad). Damage is MGSV's own:
  each round counts as the share of a soldier's life it takes in MGSV, so a soldier dies to as many hits as in MGSV
  wherever he stands (evening: 5 assault-rifle body hits of 46 HP killed a 219 HP Gatefront soldier, and one assault-rifle
  headshot (230) killed another: a headshot takes at least a whole soldier's life), and big enemies count as many soldiers
  (a field boss: ~10-20% per magazine, computed, not yet tried on one). Rockets fly visibly from the muzzle -- a small
  dark body with a bright trail, 16 m/s off the launcher and 60 m/s once its motor burns after 1 s -- and explode where
  they hit (evening: 8.6 m in 0.55 s, 139 m in 3.2 s, the end of the range at 151 m in 3.4 s); grenade rounds fly at
  80 m/s. One ZORN-KP grenade round kills a Gatefront soldier, the S1000's stun rounds stagger, CQC (RT next to an enemy) strikes,
  enemies react, guard, aggro and drop runes; the full damage lands 2-3 frames after the shot. Their hits drain Snake's
  life (a soldier's spear: ~8%), and Elden Ring's HP bar shows it: it drops with Snake's life and climbs back as MGSV
  regenerates it. When Snake dies he throws himself down where he fell, YOU DIED, the grace or the Stake of Marika
  respawns him, life full.
- **D-Walker.** Deployed beside Snake, follows him on foot to a spot at his left (live: median 5.2 m beside a running
  Snake, 2-5 m when he stops), fades out when it gets between the camera and him, stands on Elden Ring's ground. Y
  next to it mounts it: ride at ~7-9 m/s through Elden Ring, its flamethrower (RT; LT+RT in its cockpit view) burns
  enemies (fire damage), drawn with Elden Ring's Dragonfire flames, seen from behind and from the cockpit (evening).
  **Orders** (see [How to play](../README.md#how-to-play)): Elden Ring's corner shows each order MGSV took; a mode you choose stays
  (live: Search Mode, Snake ran 68 m away, the walker stayed), and the whistle brings it back from anywhere (68 m: placed
  21 m away by MGSV, beside Snake 6 s later). Following never changes its mode or makes it talk (10 min of play: 13 orders,
  13 mode changes, 12 voice lines, 0 whistles of ours).
- **Elden Ring's things.** Y is Elden Ring's "event action" while its prompt shows (with pad glyphs: "Y: Retrieve lost
  runes"): rest at a grace (its menu gets the pad, B leaves), pick up items, retrieve lost runes. Start = Elden Ring's
  menu, View = its map; fast travel works and Snake is back after the load. MGSV never reacts to those presses.
- **MGSV's iDroid.** Hold View (0.4 s) and MGSV gets it; while MGSV's own menu is open it has the whole pad and Elden
  Ring nothing. Its pause menu keeps only Controls and Options; the iDroid can't start missions or call the helicopter,
  but supply drops work (live: a BURKOV TB tranquilizer pistol ordered through the iDroid, two darts in a torch-bearing
  Limgrave soldier's body put him to sleep, HP untouched).
- **Tranquilizer darts** (evening). Snake's handgun is a WU S.PISTOL with darts (MGSV loads it with the mission: he draws,
  aims, scopes with RB and fires it). Two darts in a Gatefront soldier's body (136 sleep build-up each against his 226)
  put him to sleep, HP untouched, from the hip and down the scope. Every lethal round tells Elden Ring MGSV's own damage
  (a share of a soldier's life: an assault rifle round 21%, a sniper round 71%, the FB MR rocket 135%;
  [docs/COMBAT.md](COMBAT.md)); scoped shots leave MGSV exactly along the sights.
- **Aim down the sights** (evening). RB in first person: Snake's gun and its sights are drawn in front of Elden Ring's
  camera (they used to fade out there), and scoped shots hit (two scoped darts put a soldier to sleep).
- **The cardboard box** (evening). Snake's first box carries MGSV's anime pin-up on its lid, drawn in Elden Ring; an
  enemy that sees it within 15 m stops and stares for 4 s (seen six times at 7-15 m, a Gatefront soldier among them).
  Keeping still in it puts Elden Ring's stealth effect on the hidden Tarnished; moving takes it off. A soldier 6 m away
  left the still box alone for 10 s, but he ignored Snake outside it as well, so the proof is still the earlier fake-MGSV
  test (a soldier ignored the still box for 9 s and attacked within 3 s without it).
- **Break pots, crates and carts** (evening). An assault-rifle round broke a Limgrave wooden cart (Elden Ring's own
  breaking, as for the Tarnished: its top went from 1.0-1.7 m to 0.1 m); walking into the cart, Snake stopped at it,
  never inside. Earlier (fake MGSV) a CQC strike at nothing and a rocket's blast broke carts and crates too.
- **Only Snake shows** (evening). The hidden Tarnished's own weapon no longer lies at Snake's feet: it is hidden with
  the rest of him (his base transparency).
- **Ten minutes of continuous play** (evening): running, panning, stances, the box, tranquilizer shots, assault-rifle
  bursts, walking up to D-Walker: 0 warnings or errors in either plugin's log, 0 unstucks; Snake died once (to the
  soldiers a burst stirred up) and was back 30 s later.
- **Sound.** MGSV's music, ambience and radio are muted; Snake's footsteps (-37 dBFS) and shots (-39, peaks -12)
  play at about Elden Ring's level (-35).
- **MGSV can go away.** Close or crash MGSV: "MGSV stopped -- Elden Ring has the controller", the Tarnished is
  back at once. `pr launch --mgsv-only` brings Snake back (~2 min, live) without touching Elden Ring.

## Known issues

- Getting on D-Walker with the test pad: in the 10-minute play, Y right after walking up to it (1.4-1.6 m) never mounted
  it (11 tries); on their own, the same walk and press mounted it (twice, one after the play). Not understood yet.
- Elden Ring's grass is drawn over Snake's models even where it stands behind them (seen on the box in tall grass).
- D-Walker's footsteps are loud: -21 dBFS (peaks near clipping) vs Elden Ring's -35. MGSV's Wwise is found, but
  per-object volume, sends and attenuation calls have no audible effect so far ([docs/GUEST.md](GUEST.md) 18.2).
- D-Walker still answers ~2-3 s late: while Snake runs it is ~5 m away (median; up to 7-13 m before it is moved
  closer), 3 m beside him 2 s after he stops.
- MGSV's own effects (explosions, muzzle flashes, smoke, flames) are never drawn in Elden Ring; blasts are drawn with Elden
  Ring's pot effects instead (Fire Pot fireball, Holy Water Pot flash, Sleep Pot mist, Fetid Pot cloud; seen live,
  [docs/COMBAT.md](COMBAT.md) §9), D-Walker's flamethrower with Dragonfire flames. The smoke looks more like brown
  dust than MGSV's white smoke. Muzzle flashes: none.
- The grenade in flight is a simple ball, a rocket a small dark ball with a glowing trail, not MGSV's models. CQC
  throws/chokes need a target MGSV doesn't have. C4's detonator button in MGSV wasn't found (RT, RB, B, X, Y, stick
  clicks, D-pad: nothing), so placed C4 can't be set off yet.
- Breaking things: sturdy ones (defence 90: Limgrave's wooden palisade fences) don't break even in a blast; a few low
  crates stayed up under CQC strikes; pots that break when the Tarnished just walks into them weren't near the test
  spots (UNVERIFIED for Snake: shooting, striking or blowing them up works as for crates). [docs/HOST.md](HOST.md)
  "Breakables".
- The box: an enemy that already spotted Snake keeps coming (Elden Ring's chase isn't reset).
- Long clouds (a smoke grenade's ~60 s): when Elden Ring's effect pool is full and refuses our spawns (a session 4 smoke
  cloud lost its effect that way for a minute), refreshes now wait up to 10 s instead of piling on, and the log warns;
  clouds also follow Elden Ring's origin shifts. Not re-tested live (the evening pass couldn't throw).
- Snake and D-Walker are lit with Elden Ring's world light set (0) since its character sets (3, 4) light nothing while no
  Elden Ring character is on screen (they turned pitch black); the world set lacks a character's rim light, so they look
  a little flatter than the Tarnished ([docs/HOST.md](HOST.md)).
- Elden Ring's own frame times show a single 36-40 ms frame one to six times per 10 s in the open world (the pacing probe,
  both games running); each is a small visible step in a pan. The evening pan also had two one-frame yaw steps of 0.85
  and 1.6 deg (frames of ~19 ms) and one 2 m camera pull-in by collision. Not investigated yet.
- Regeneration after Elden Ring's hits now follows MGSV's own curve (fixed; Margit kills a Snake who stands still in ~12 s);
  life can tick up by up to ~2% for a moment before the curve's first write after a hit.
- D-Horse: no treadmill while riding (nothing found moves the horse in MGSV), so MGSV's own map limits a ride, and after a
  far dismount the horse jumps back in Elden Ring and gallops in. Galloping off a ledge hurts as a fall. Quiet: untested.
- D-Dog's coat is sometimes drawn in false colours (orange/green; normal grey at other times), and D-Walker right next to the
  camera in magenta/orange (MGSV's near-camera fade): materials the capture doesn't map yet. The cardboard box looks very dark.
- Low ceilings: Snake stops ~0.2 m short of an underside lower than him (his ahead probe), also at table edges and overhangs.
- Fulton: Elden Ring's balloon goes up ~5 s after Y is let go (it waits for MGSV's balloon to lift the stand-in); Elden Ring's
  sleep lasts ~50 s, and a soldier that wakes meanwhile can't be fultoned.
- Full proxies (`[Proxies] Enabled`, off by default): with the real Elden Ring they set MGSV on alert (an MGSV reinforcement
  shot Snake on the apron) and can't stand at heights the apron doesn't have: not for play yet.
- Ladders: walking on into the hole at a ladder's top drops Snake down the shaft (face it and press Y on "Descend").
- Flasks, spirit ashes, Torrent and jumps need Tarnished mode (ladders: Snake's Y hands the climb to Elden Ring).
- Skipping a boss's intro cutscene: fixed (e226ea5, 6b6ac58) and unit-tested, not re-tried live (Margit's plays once).
- MGSV's goggles on Snake's head are drawn with a noisy texture on their lenses.
- MGSV advances its game one fixed step per frame and still drops to ~54 frames/s while the camera turns: Snake's game
  then runs ~10% slow (60 when it doesn't). Snake's root moves in whole animation keys (2 or 3 per frame running, MGSV's own view steps the same);
  the puppet's motion is smoothed over them, his limbs are not.
- Windows players: built, not yet run on Windows ([docs/WINDOWS.md](WINDOWS.md)).
