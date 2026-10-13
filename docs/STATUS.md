# Status (2026-10-12)

The detailed record of what has been tested live, and how. The short version is in the [README](../README.md).

Tested with both games running (`pr launch`, Limgrave: Gatefront, Agheel Lake, the Stake of Marika). The last pass
(2026-10-12 evening: ~2.5 hours with both games, 10 minutes of it continuous play) drove the controller through the
plugin's test pad, with no player; "evening" below marks what it re-tested.

![Snake and D-Walker at Gatefront, lit by Elden Ring, MGSV's HUD on top](screenshots/snake_walker_gatefront.jpg)
![Riding D-Walker into Gatefront Ruins](screenshots/riding_dwalker_gatefront.jpg)

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
- MGSV's aim assist still knows its own airport soldiers (disabled, never drawn): aiming near where one stands in MGSV,
  the aim snaps toward him and off the Elden Ring enemy you aim at (seen with the launchers and the test pad's aiming).
  Their markers are now cleared every 5 s (`[Boot] ClearMarkersSec`); the snap isn't.
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
- An Elden Ring menu opened from the keyboard (Esc) isn't noticed: MGSV's HUD layer and the throw markers stay on top of
  it (the pad's Start is).
- Snake and D-Walker are lit with Elden Ring's world light set (0) since its character sets (3, 4) light nothing while no
  Elden Ring character is on screen (they turned pitch black); the world set lacks a character's rim light, so they look
  a little flatter than the Tarnished ([docs/HOST.md](HOST.md)).
- Elden Ring's own frame times show a single 36-40 ms frame one to six times per 10 s in the open world (the pacing probe,
  both games running); each is a small visible step in a pan. The evening pan also had two one-frame yaw steps of 0.85
  and 1.6 deg (frames of ~19 ms) and one 2 m camera pull-in by collision. Not investigated yet.
- A boss's hits were not re-tested after the control fix: ordinary enemies' hits drain Snake's life (27 hits, then YOU
  DIED, live), and the player's session 3 shows a boss killing Snake.
- Elden Ring heals (resting at a grace, regenerating talismans) are sent to MGSV (`kHEvHeal`); the MGSV plugin now
  applies them to Snake's life (not yet tried with both games). Flasks can't be drunk while Snake plays.
- MGSV advances its game one fixed step per frame and still drops to ~54 frames/s while the camera turns: Snake's game
  then runs ~10% slow (60 when it doesn't). Snake's root moves in whole animation keys (2 or 3 per frame running, MGSV's own view steps the same);
  the puppet's motion is smoothed over them, his limbs are not.
- Windows players: built, not yet run on Windows ([docs/WINDOWS.md](WINDOWS.md)).
