# PhantomRing combat (phase 3)

DESIGN.md §8 made concrete: Snake's shots, CQC and explosions hurt Elden Ring characters through Elden
Ring's own pipeline; Elden Ring's hits drain Snake's MGSV life; Snake's death is the Tarnished's death
(YOU DIED, grace respawn) and the respawn refills Snake.

Status: **live with both games** (2026-10-12, §8 has the numbers): real MGSV shots, CQC and D-Walker's
flamethrower hurt, stagger and kill Elden Ring enemies (kills give runes), Elden Ring's hits drain Snake's life,
Snake's death is YOU DIED and the respawn refills him. Every game address is verified against the exe on disk
(`verify_er_addresses.exe`: 67/67). Every param row and field offset the host patches is checked against this
game's `regulation.bin` (`test_host_combat`). The original live test plan is §6.

## 1. What happens where

| Event | Guest (MGSV) | Host (Elden Ring) |
|---|---|---|
| a round leaves Snake's gun | ShotSolver hook: `kGEvFire` (muzzle, direction, weapon, ammo kind, pellets, `pad[0]` = bulletId \| 0x80000000 explosive) | anchor-mapped ray: ER's world ray gives the first wall, ray-vs-capsule over every character nearby gives the first body before it → hit effect → applied (§2) |
| D-Walker fires | `kGEvBuddyFire`; its flamethrower (no rounds): ~8 events/s while it burns, `pad[1]` = reach (12 m) | same, × `buddy_mult`; flame: a stream as long as the reach, bodies within 0.6 m of it burn, **fire** damage (ER's fire absorption, its burning VFX) |
| CQC strike | attack-serial poll: `kGEvMelee` (strike sphere, facing, kind) | the nearest hostile the sphere touches, or in reach (`melee_range`) inside the arc (`melee_arc_deg`) in front of Snake |
| grenade/rocket | `kGEvThrow` / `kGEvPlaced` (v5), then `kGEvExplosion` (pad[1] = its id; kind = lethal/sleep/stun/smoke), or an explosive round's hit point (gun-fired) | our copy flown through ER's world / the mine kept where it lies (HOST.md); every hostile inside the blast, damage falling to 25% at the edge; ER enemies near a mine -> `kHEvDetonate` |
| ER hits the puppet | (receives `kHEvHurt`) life −= amount × `LifeScale`; camera shake + blood (`HurtFeedback`) | `ReceiveDamage` hook lets ER compute the hit at full HP, puts the HP back and sends `kHEvHurt`: the share of the Tarnished's max HP it took, as a share of Snake's life (`hurt_share`; melee/projectile by attacker distance, direction in MGSV world); poison/fall/other HP loss: HP-delta check, same share |
| a hard landing | (receives `kHEvHurt`, kind fall) | kinematic motion: host computes it from the landing speed (`fall_damage`); ER's own fall damage for the same landing is ignored |
| Snake's life would hit 0 | kept at 1, `kGEvDied` once (latched) | NoDead/NoDamage dropped, `ChrIns::Kill` → YOU DIED → respawn → `kHEvRespawn`; kill verified (retried, else Snake is refilled) |
| ER respawn | refill to lifeMax, unlatch | `kHEvRespawn` on dead → alive |
| HP bar | — | puppet HP = max(1, round(max HP × life / lifeMax)) every frame (`mirror_life`); hits measured at full HP, the mirrored HP put back |
| ER heals the puppet (grace rest, regeneration) | (receives `kHEvHeal`, v3) life += amount × `LifeScale` -- **not yet in mgsv/** | HP above last frame's mirror -> `kHEvHeal` with the same share of Snake's life (`forward_heals`) |

## 2. How a hit is applied in Elden Ring (`er/Combat.cpp`)

**Targets.** `WorldChrMan`'s by-distance list; each character is an upright capsule from its feet with
the physics module's hit height/radius (`CSChrPhysicsModule+0x2F0/+0x2F4`, loaded from NpcParam
`hitHeight/hitRadius`; `+0x2E0/+0x2E4` second; `[Combat] capsule_*` last). **Hostility** is Elden Ring's
own: `ChrIns::GetTeamType` (team + SpEffect overrides, `0x3F1C90`) for both, then the static
`CSTeamTypeBase* [79][79]` relation table that `0x51B5D0` indexes (`0x3B283F0`): hostile when the entry
is `CSTeamTypeEnemy` or `CSTemaTypeRival` (sic). Read offline, the Tarnished (team 1) may attack teams
6–9, 11, 12, 16–22, 24, 25, 27, 29, 31–45, 48–77, and not 0–5, 10, 13–15, 23, 26, 28, 30, 46, 47.
`[Combat] targets = all` ignores this.

**Damage, poise, sleep** (`er/CombatLogic.cpp`, unit-tested):

- **Damage is counted in MGSV soldiers' lives** (2026-10-12; `combat::ShotEffect`): a hit is worth a share of one
  standard MGSV soldier's life -- **MGSV's own number** when the guest sends it (protocol v8: `kGEvFire` pad[1] = the
  round's lethal damage on a body hit / the soldier's life × 10000, per pellet; × `share_mult`), else the class value
  (handgun 13, magnum 30, rifle 17, battle rifle 20, MG 15, sniper 55, buckshot 6/pellet, slug 40, heavy 60, explosive 70,
  unknown 15, stun 1 (+ 40 poise), tranquilizer 0) / `units_per_life` (40: a handgun round 0.33 of a life, a rifle round
  0.43, a sniper round 1.4). Stun, flame and explosive rounds keep their class rules.
- × the target's **reference HP**: its max HP for an ordinary enemy, so a soldier dies to as many hits as in MGSV
  wherever he stands in Elden Ring (Elden Ring scales max HP with the area; measured against the Tarnished's own max HP
  -- read every frame -- a late-game soldier is as ordinary as a Limgrave one). Bigger ones are worth more lives:
  bigness t = 0 up to `big_from` (1.5) × the Tarnished's max HP, 1 from `boss_from` (5) ×, log-interpolated between;
  reference = max HP / `boss_lives`^t (45: a 3000 HP field boss against a 522 HP Tarnished takes 14 per assault-rifle
  round with MGSV's own share, 0.2115 -- a magazine 14%; live, Margit (4174 HP vs 572): 30 rounds 607 HP = 14.5%. The old
  70 was set for the class table's rifle, 0.425 of a life, twice MGSV's real share: 9.8% a magazine on Margit). Bosses are recognised by their HP, not by the boss bar (its entries, CSFeMan
  +0x5BF0, are unverified). Elden Ring's guard reductions still apply (the top-up scales by what got through, below).
- Headshot: hit above `head_fraction` of the capsule height: × `headshot_mult` (4.5 on an ordinary enemy: MGSV's
  headshots kill) down to `headshot_mult_boss` (1.5) with t, and at least a medium flinch. A lethal headshot (not stun,
  not flame) on an ordinary enemy also takes at least `headshot_min_lives` (1.05) of a soldier's life, × (1 - t): none
  on a boss. Without it the assault rifle's own share (0.2115 × 4.5 = 0.95 of a life) left a Limgrave soldier at 11 HP.
- Targets met with MGSV's own shares: assault rifle 5 body hits / 1 head on a Limgrave soldier (219 HP: 0.2115 -> 46 a
  hit; head 230), handgun 4, sniper (0.71) 2 / 1, a boss ~15% a magazine (tested; live on Margit, §8). VERIFIED live with both games
  (2026-10-12 evening): five body hits `hp 219 -> 177 ... 35 -> 9 (wanted -46)` and dead; one headshot `HEADSHOT ...
  bullet 230` on a 219 HP soldier: dead.
- Tranquilizer: no damage, sleep build-up: `tranq_share` (head: `tranq_share_head`) of the target's own sleep resistance
  (`tranq_sleep` / `tranq_sleep_head` when it can't be read). CQC choke: `choke_share`. Sleep gas: `gas_share` per MGSV
  report (every 0.5 s). Stun rounds: a token of damage, heavy poise, the large flinch. Stun and sleep never kill (`nonLethal`:
  our part of their damage stops at 1 HP, and so does Elden Ring's: `combat::NonLethalCorrection` caps the hit bullet's
  AtkParam correction so that even kNonLethalSpread (1.6) × a full hit of that enemy's reference HP leaves 1 HP -- 0% at
  2 HP or less, the sleep correction when there is no damage at all. A stun grenade's fixed 15% used to kill a target at
  1 HP: Elden Ring never clamps its own hits).
- CQC: punch/kick damage + `melee_strike_poise`; throw: `melee_throw_poise`, large flinch.

**Routes** (`[Combat] route`):

- `bullet`: a bullet **owned by the Tarnished** is spawned 0.6 m in front of the hit point, aimed through
  it, with the victim as its target (`CSBulletManager::SpawnBullet 0x3A2CB0`; spawn data per
  fromsoftware-rs: owner handle, bulletId, magic/goods/behavior/dummy-poly −1, target handle, forward
  vector, position; `0x38E220`/`0x38E550` confirm that magic = goods = −1 means a plain bullet with
  no spell or item scaling). The row is `bullet_id` (10170000, the Throwing Dagger's); its AtkParam_Pc row
  (10170000) gets this hit's `atkPhys` or `atkFire` (flame), the matching corrections, `atkStam` and
  `atkSuperArmor` (poise, correction 100) and `dmgLevel` written just before the spawn.
  The flight effect is hidden (`bullet_hide_sfx`). Elden Ring then does the rest: hit reaction, poise,
  stagger, aggro on the Tarnished, guard, its own defence and absorption, runes for a kill. Those rows only
  change while the puppet is active (the player can't throw daggers then), and every byte goes back when the
  puppet stops. Two bullets in flight in the same frame share the last numbers written (one target per
  shot, so this only blurs shotgun blasts or explosions on a crowd).
  - **Elden Ring ignores `atkPhys` for this bullet** (VERIFIED live): the row's `atkBehaviorId` is 0, i.e. the
    attack comes from the Tarnished's own weapon × the row's correction. A 38-, 190- and 714-attack bullet all
    dealt ~42 HP to a 219 HP Gatefront soldier (63 on a counter hit), a blocked one 5. So **`top_up`** (on):
    once ER has applied its hit (`verify_ms`, 300), the rest of the wanted damage is added with
    `CSChrDataModule::ChangeHp` / `ChrIns::Kill` on the frame after ER's hit lands (pending hits are looked at every
    frame; VERIFIED live: 31-49 ms after the spawn, it was `verify_ms` = 300 ms for every hit) -- scaled by how much of a normal hit got through
    (`combat::TopUp`: ER's largest damage of one of our bullets on this kind of enemy is the reference; half of
    it or more counts as unblocked, less as guarded and lets that share through). Kills this way give runes
    (VERIFIED: +64 for a soldier). Wanted damage below the reference (stun rounds, flame) flies with a
    proportionally lower correction so ER's own hit never exceeds it (a stun round: ER 1 HP + 4 topped up).
  - Blasts (explosive rounds, `kGEvExplosion`) come down on each target from above, not from the blast centre:
    a raised shield faces the shooter.
  - **One row for every bullet** (`combat::MayFly`): all hit bullets share one patched AtkParam_Pc row, and ER
    most likely reads it when the bullet lands (UNVERIFIED), so a bullet only flies next to bullets in flight with the
    same numbers, and only one with damage per target (the top-up measures a target's HP loss against its pending
    bullet). The others take the `hp` route at once (`N hp (M while a bullet flew)` in `combat`): a blast's
    other targets and a shotgun's other pellets get their damage but no ER reaction. An HP-route hit on a target
    whose bullet already landed keeps what ER had dealt for the top-up.
- `hp`: `CSChrDataModule::ChangeHp` (with Kill's own arguments) or `ChrIns::Kill` at 0. Always lands. No
  reaction, no aggro. A lethal hit first asks `ChangeHp(0)`: Elden Ring holds an event-immortal boss or NPC at
  1 HP there (NoDead bits, data+0x19A bit 7, SpEffect states 0x8F/0xEB; VERIFIED in Ghidra), and only what reaches
  0 is killed (`ChrIns::Kill` marks anything dead). Same for the top-up and the fallback.
- `auto` (default): `bullet`, verified per hit. Did the target lose HP (or die) within `verify_ms`? Until
  one bullet lands, each miss also gets the `hp` damage. `verify_failures` misses in a row switch to `hp`
  for the session (`combat route bullet` retries). Once a hit lands the route is **confirmed** and misses
  stay misses (a block or a dodge is ER's call). Landed hits also measure how much of the wanted damage
  ER actually dealt (`combat` shows it); tune `bullet_damage_mult` with that number.
- **Sleep rides on the hit bullet** (player report: "sleep grenade did not work at all"): its BulletParam `spEffectId0..4`
  get rungs of ER's own sleep build-up ladder, SpEffectParam 25000 + n − 1 = n × 10 (10..200; stateInfo 436 = sleep,
  checked at run time; more than 200 takes several slots), as Elden Ring's own sleep bullets do (Bullet 2270260 -> 25006;
  the Sleep Pot's mist -> 500640). A sleep-only hit flies with AtkParam correction `sleep_correction` = 0: no damage, no
  flinch, and the build-up still lands. VERIFIED live: `ChrIns::ApplySpEffect` of a build-up row (25019 twice, the
  self-targetable 1885 three times) built **nothing** on a soldier (the old route: every sleep the logs counted since
  phase 3 did nothing), and the AtkParam's own `spEffectId` slots didn't either; the bullet's slots do.
- **The amount is a share of the target's resistance** (`CSChrResistModule`, `[modules+0x20]`, VERIFIED live on a
  Gatefront soldier): `+0x2C` i32[7] the thresholds (poison, rot, bleed, death, frost, **sleep**, madness: 169 169 226 999
  226 226 2015 -- NpcParam's 112 with Limgrave's scaling), `+0x10` i32[7] what is left before each procs (the sleep entry
  fell 226 -> 151 -> 4 under the gas, then the soldier slept and it reset to the raised threshold 268: `resistCorrectId`).
  999 = immune (none given). So 2 body darts (0.6), 1 head dart (1.1), a choke (1.1) or ~3 s of gas (0.2 per report) put an
  enemy to sleep whatever its area or how often it already slept.
- **One blast, one set of numbers** (`combat::StunBlastEffect` / `GasBlastEffect`): every hit bullet shares one patched row
  and only identical numbers fly together (`MayFly`), so a stun grenade gives everyone inside the centre's stagger (MGSV's
  flash doesn't fall off) with a fixed correction (15), and a sleep-gas puff one build-up for everyone: `gas_share` of the
  largest known resistance among the enemies inside (immune ones still get nothing). Before, only the first target's
  bullet flew; the others went to the HP route (no stagger) or lost the sleep. VERIFIED live (both games, three Limgrave
  soldiers of resistance 169 and 197 in one cloud): +39 each per puff, 68 sleep build-ups, 0 lost, three of them slept.
  `HitKey` keeps the sleep sum in 10 bits (up to 1023; the default ladder tops at 1000); more never shares the row.
- Rate limit: `max_hits_per_second` token bucket. Drops are counted.

### Damage that syncs: the damage ladder (`[Combat] damage_route = speffect`, off by default; the rows arrive live, the split needs work: §10; roadmap §4 C1)

The top-up above is a direct HP write. In a networked session (Seamless Co-op) the session host owns its enemies' HP
(DESIGN §14c), so a top-up on a guest's machine never reaches the host and the enemy's two copies disagree. With
`damage_route = speffect` the whole hit is Elden Ring's own: the hit bullet carries the wanted damage as **fixed-damage
SpEffect rows** in its BulletParam `spEffectId0..4`, the slots the sleep ladder already rides in (§2 "Sleep rides on the hit
bullet"), so whatever carries the hit to the enemy's owner carries the damage too. Code: `combat::DamageLadder`,
`combat::NativeDamage` (`er/CombatLogic.{h,cpp}`, unit-tested), `er/Combat.cpp` (`LadderReady`, `Apply`, `Tick`).

- **The ladder.** 36 SpEffectParam rows from `damage_speffect_base` (20010640), one per decimal digit of each place: row
  base + 9p + (d - 1) deals d x 10^p HP (`changeHpPoint`, positive = damage like Elden Ring's own hemorrhage rows; once:
  `effectEndurance` 0). A hit carries one row per non-zero digit, so 1..9999 HP is exact in at most 4 rows (`max_damage` is
  9999). When sleep rungs leave fewer free slots the amount rounds to that many significant digits (half up: 1234 in two
  slots = 1200); a lethal hit carries no sleep, so this does not happen today.
- **The carriers.** Elden Ring has no ladder of fixed-damage rows (its fixed-damage rows are 10, 20, 25, 50, 80, 100, 140,
  160, 300 and 700 HP, each with its own purpose), so 36 **blank** rows are written: 20010640..20010679 are 40 rows identical
  to Elden Ring's default row, referenced by no i32 in any of the 183 param files of this regulation (`test_host_combat`
  checks both, and that each would equal 99441, a plain "10 HP once" row, but for its damage, priority and target bits).
  While Snake plays each gets its `changeHpPoint` and the target bits of the sleep rungs that our hit bullet VERIFIABLY lands
  on enemies (0x7E / 0x03: everyone but the shooter); at run time every row must still be blank first, or the ladder stays
  off and the log says which row. They are put back with the bullet rows when the puppet stops. The values are fixed, so
  every PhantomRing player's game holds the same rows; a player without PhantomRing would not (C4: everyone runs it).
  `isDisableNetSync` stays 0. (EMEVD/ESD scripts can't be read offline: that nothing applies these ids is checked for params only.)
- **The split.** The bullet still flies, with a small AtkParam correction: Elden Ring's own hit (the Tarnished's weapon x
  the correction, §2) is aimed at `speffect_bullet_share` (10%) of the wanted damage and still gives the flinch, the poise,
  the aggro and the guard reaction. Its 100% hit on that kind of enemy (the reference) is learned from landed hits (ER's part /
  correction, a moving average); until one has landed the bullet flies with `speffect_first_correction` (10%) and the ladder
  carries all of the wanted damage (the first hit is ~4 HP over). The ladder carries wanted - ER's expected part. Stun and
  sleep hits keep the top-up (their part must never kill).
- **Guard.** A hit whose ER part is under half the expected one is guarded (as the top-up's rule; only judged when ER's part
  is 4 HP or more, so its rounding of a boss's ~2 HP part is no guard). Whether Elden Ring also drops a guarded bullet's
  SpEffects is not known yet, so it is learned on the first guarded hit and logged (`damage ladder on a guarded hit: ...`):
  if the rows' damage still arrived (*applies*), the target's next hits within `speffect_guard_ms` (1.5 s) carry only the
  share of a normal hit its shield let through (5 of 42: 12%), and an unguarded hit clears that; if it did not (*skipped*),
  Elden Ring blocks it itself and nothing is scaled. Either way the first guarded hit of a raised shield is decided by
  Elden Ring.
- **Did it arrive?** Each landed hit's HP loss is split into ER's part and the ladder (the rows are exact). Until one
  hit's rows have arrived the ladder is unconfirmed: a hit whose rows did not arrive (and was not guarded) is topped up as
  before, and `verify_failures` (3) such hits in a row give the ladder up for the session (`damage_route topup`, logged;
  `combat damage_route speffect` retries). Once confirmed, a missing ladder is counted, not topped up. Only rows that
  arrived confirm it: a guarded hit whose rows did not arrive is no evidence either way (`combat::ReportLadder`, review
  2026-10-14: it used to confirm the ladder, so with rows that never work one shield left every later hit at ER's ~10% for
  the session), and "Elden Ring drops it on a guarded hit" is learned only once the ladder is confirmed.
- **Limits.** Everything else is unchanged: hits that can't take a bullet (another bullet holds the row, `combat::MayFly`; a
  blast's other targets; a shotgun's other pellets; Elden Ring refusing the spawn) still take the HP route, which does not
  sync. Elden Ring's absorption does not apply to the ladder (the top-up ignored it too). Kills by the ladder should give
  runes as hemorrhage kills do (UNVERIFIED).

**Params at run time** (`er/Params.cpp`): `SoloParamRepository` (`0x3D85F58`, vtable
`CS::SoloParamRepositoryImp`) → holder (`+0x80 + 0x48·i`) → `CS::ParamResCap` (vtable checked, name at
`+0x08`) → `+0x80` FD4ParamResCap → `+0x80` ParamFile. A table is used only if its resource name (or,
failing that, fromsoftware-rs's holder index), struct name and row stride all match. Offsets:
`tools/re/er_params.py fields <STRUCT>`.

### Progression: Elden Ring's levels and weapons for Snake (`[Progression]`, off by default; roadmap 2.12)

Snake's damage is counted in MGSV soldiers' lives against each target's own HP (above), so it never grows with the
Tarnished: five assault-rifle body hits kill any ordinary enemy and a magazine takes ~15% of any boss, at rune level 9 or 150.
The option lets Elden Ring's progression count for that (`er/Progression.{h,cpp}`, the arithmetic in `er/CombatLogic.cpp`
`ProgressionFactor`, unit-tested in `tests/test_host_combat.cpp` `TestProgression`):

| `damage_scaling` | Snake's lethal damage x |
|---|---|
| `off` (default) | 1: MGSV's own |
| `level` | (rune level / `level_reference`) ^ `level_exponent`: with 20 and 0.5, level 10 x0.71, 15 x0.87, 20 x1, 40 x1.41, 80 x2, 150 x2.74 |
| `weapon` | 1 + `weapon_bonus` x upgrade / its maximum, for the right-hand armament in use: +0 x1, +12 x1.72, +25 (a somber +10) x2.5 with 1.5 |
| `both` | level x weapon |

The factor is kept within `min_factor` .. `max_factor` (0.5 .. 3) and multiplies every lethal hit Snake and his buddy land --
rounds, CQC, blasts, D-Walker's rounds and flames (`HitRules::progression` in `FromLives`); sleep and stun build-up and the stun
blast's fixed token stay as they are (the headshot's kill guarantee is counted in lives, so it scales too).
Re-read twice a second while Snake plays; every change is logged (`progression (level): rune level 15, right hand 18000000 (+0 of
25) -> Snake's damage x0.87`). `pr er progression [off|level|weapon|both]` shows the readout and switches the mode live.

**Vigor needs no option.** Elden Ring's hits already cost Snake the share of the Tarnished's max HP they take (`hurt_share`,
`combat::HurtForGuest`): a Margit hit of 300 is 52% of a 572 HP Tarnished's bar and so 52% of Snake's life; with 1200 HP it is 25%
of either. Every Vigor point, HP talisman or Crimson Tears upgrade makes Snake exactly as much tougher as the
Tarnished; a "Vigor -> Snake's max life" option would count it twice.

What is read (each checked, else the factor stays 1 and the log says why once; fromsoftware-rs `player_game_data.rs`):
`[PlayerIns+0x580]` PlayerGameData (RTTI `CS::PlayerGameData`), +0x14 max HP (must equal the data module's), +0x3C eight u32
attributes Vigor .. Arcane, +0x68 the rune level (must equal their sum - 79, Elden Ring's rule), +0x31C its ChrAsm (EquipGameData
+0x2B0, +0x6C): +0x10 the right hand's slot in use (0..2), +0x7C the equipment param ids (right hand slot s at [1 + 2s]). A weapon
id is its EquipParamWeapon row ((id / 100) x 100: base + infusion) plus the upgrade (id % 100); the maximum is how many
ReinforceParamWeapon rows its `reinforce_type_id` (+0xDA) has (25 for most, 10 for somber: 357 and 231 base weapons in this
regulation, unit-tested against it; Longsword type 0 -> 25, type 2200 -> 10). Both tables bind through `params::Table` (struct
name and row size checked: 0x298, 0x80). VERIFIED live 2026-10-13 (the player's save): `rune level 15 (Vig 17 Min 10 End 13 Str 16
Dex 13 Int 9 Fai 9 Arc 7), max HP 572` (both cross-checks pass), `right hand: slot 1 weapon 18000000` (the Halberd in his hand on
the screenshot) `+0 of 25`; D-pad right as the Tarnished -> `slot 0 weapon 2000000` (the Longsword in his hand); the other ids
are the Vagabond's (Heater Shield 31330000 left, unarmed 110000 elsewhere). The factor on real hits (fake MGSV's Snake on Fort
Haight's tower, `combat shoot rifle` with MGSV's share 0.2115 at a 167 HP enemy's body, `pr er progression <mode>` between the
shots): off `bullet 35`, level `bullet 31` (x0.87), weapon `bullet 35` (+0: x1), both `bullet 31` -- as computed. Again with the
real MGSV (2026-10-13 morning, combined run; a 167 HP soldier in the Castleward Tunnel, `combat shoot rifle` at his body, the
mode switched live between shots): off 35, level 31, weapon 35, both 31 (`progression (level): rune level 15, right hand 18000000
(+0 of 25) -> Snake's damage x0.87`), and `progression off` back.

## 3. Ini flags

### Host: `PhantomRingHost.ini`

| Key | Default | Meaning |
|---|---|---|
| `[Puppet] hit_hook` | **true** (was false) | ER hits on the puppet are caught in `CSPlayerDamageModule::ReceiveDamage` (instance-vtable swap, slot 7) and cancelled: no HP loss, no stagger. Forwarded as `kHEvHurt`. Set false if the first enemy hit misbehaves. HP-delta detection stays on for poison and falls. |
| `[Puppet] kill_on_guest_death` | true | Snake's death kills the Tarnished (YOU DIED). false: no ER death; the host refills Snake at once (`kHEvRespawn` 1), so his collapse ends and later deaths are reported again (UNVERIFIED live). |
| `[Puppet] mirror_life` | true | HP bar = Snake's life fraction. |
| `[Puppet] forward_heals` | true | Elden Ring heals on the puppet raise Snake's life by the same share (`kHEvHeal`); false = the mirror puts the HP back. |
| `[Combat] damage` | true | Off: shots are only traced and logged. |
| `route` | auto | auto \| bullet \| hp (§2). |
| `targets` | hostile | hostile (ER's team relation) \| all. |
| `bullet_id`, `bullet_hide_sfx`, `bullet_offset`, `bullet_damage_mult` | 10170000, true, 0.6, 1.9 | bullet route row, flight effect, spawn distance before the hit, atkPhys = damage × this |
| `top_up` | true | complete the wanted damage with ChangeHp after ER's own (weapon-based) bullet damage (§2) |
| `damage_route` | topup | topup (ER's hit + the top-up, local only) \| speffect (the damage ladder on the bullet: Elden Ring carries all of it; UNVERIFIED live, §2 "Damage that syncs") |
| `damage_speffect_base`, `damage_speffect_digits` | 20010640, 4 | the ladder's carrier rows: 9 x digits SpEffectParam rows from base (blank in this regulation) |
| `speffect_bullet_share`, `speffect_first_correction`, `speffect_guard_ms` | 0.1, 10, 1500 | ER's own part of the wanted damage; its correction (%) before its full hit is known; how long a target seen guarding scales the ladder |
| `verify_ms`, `verify_failures` | 300, 3 | how long a bullet may take to land before it counts as a miss (a landed hit is topped up the next frame) |
| `units_per_life` | 40 | class units in one soldier's life (when MGSV sends no share) |
| `big_from`, `boss_from`, `boss_lives` | 1.5, 5, 45 | max HP / the Tarnished's: ordinary up to big_from, a boss (worth boss_lives soldiers) from boss_from (45: an assault-rifle magazine with MGSV's share takes ~15% of a boss; live, Margit, §8) |
| `share_mult` | 1 | × MGSV's own damage share (v8) |
| `head_fraction`, `headshot_mult`, `headshot_mult_boss` | 0.82, 4.5, 1.5 | headshots (ordinary → boss) |
| `headshot_min_lives` | 1.05 | a lethal headshot on an ordinary enemy takes at least this many soldiers' lives (none on a boss) |
| `buddy_mult`, `max_damage` | 1, 9999 | D-Walker rounds; cap per hit |
| `tranq_share`, `tranq_share_head`, `choke_share`, `gas_share` | 0.6, 1.1, 1.1, 0.2 | sleep build-up as shares of the target's sleep resistance (§2) |
| `tranq_sleep`, `tranq_sleep_head`, `choke_sleep`, `gas_sleep` | 80, 200, 200, 40 | the same, absolute, when the resistance can't be read |
| `sleep_correction` | 0 | AtkParam correction (%) of a sleep-only hit (0: no damage, still lands) |
| `stun_poise_mult` | 1 | stun rounds' poise |
| `sleep_speffect_base/step/count` | 25000, 10, 20 | the sleep ladder |
| `melee_punch/kick/throw`, `melee_throw_poise`, `melee_strike_poise` | 5, 7, 4, 80, 25 | CQC values (MGSV units, poise) |
| `melee_range`, `melee_arc_deg` | 1.6, 60 | CQC reach / half angle |
| `explosion_radius` | 4 | blast radius when none is reported |
| `max_hits_per_second` | 30 | rate limit |
| `capsule_radius`, `capsule_height` | 0.5, 1.8 | fallback capsule |
| `fall_damage` | auto | auto (host computes it with kinematic motion; with root_motion ER does) \| host \| off |
| `fall_hurt_speed`, `fall_lethal_speed` | 12.5, 19.8 | m/s at landing (~8 m hurts, ~20 m kills; ≥ 10% once it hurts) |
| `guest_life_scale` | 4 | must equal the guest's `LifeScale` (fall damage is sent in ER units) |
| `hurt_share`, `hurt_mult` | true, 1 | ER's hits cost Snake the share of his life they'd cost the Tarnished (44 of a 522 HP Tarnished = 8.4% of Snake's 7801); false = raw ER damage (× `LifeScale`: 2%, regenerated in under a second) |
| `[Progression] damage_scaling` | off | off \| level \| weapon \| both: Elden Ring's progression scales Snake's lethal damage (§2 "Progression") |
| `[Progression] level_reference`, `level_exponent`, `weapon_bonus`, `min_factor`, `max_factor` | 20, 0.5, 1.5, 0.5, 3 | level: x (level / reference)^exponent; weapon: x (1 + bonus x upgrade / max); the factor's range |
| `explosion_fx` | auto | explosion visuals (§9): auto \| bullet \| sfx (no verified call: off) \| off |
| `explosion_lethal/stun/sleep/smoke` | 10030001, 10035001, 10064001, `10033001 300192` | each look: BulletParam burst row [+ effect id instead of its own] |
| `explosion_flame`, `explosion_breaker`, `explosion_shrapnel` | 10700000, 10030000, 10030000 | D-Walker's flames (Dragonfire), the breakers (the flying Fire Pot) |
| `explosion_carriers` | 10048000, 10048001, 10302000 .. 10302004 | cut-content rows the looks are copied into (lethal, stun, sleep, smoke, flame, breaker, shrapnel; 4 = an older ini) |
| `explosion_max_bursts`, `explosion_per_second`, `explosion_cloud_repeat`, `explosion_cloud_life`, `explosion_ground` | 7, 40, 2.5, 6, true | bursts per blast, per second; s between two bursts of one cloud; s one cloud burst lasts; drop onto ER's ground |
| `explosion_max_live` | 48 | our effect bullets alive at once (Elden Ring's pool is shared; also capped by its own emitter/bullet counts) |
| `break_objects`, `breaker_atk`, `breaker_obj_attack`, `explosion_atk`, `explosion_obj_attack` | true, 10048000, 10, 10048001, 100 | destructible assets (§9): the object-only AtkParam_Pc rows and their atk_obj |
| `breaker_radius`, `breaker_speed`, `breaker_lead`, `shrapnel_count`, `breaker_hits_characters`, `explosion_breaks`, `breaker_per_second` | 0.15, 25, 0.6, 8, true, true, 20 | breaker size, speed, run-up; shrapnel per lethal blast; rate |
| `round_flight`, `round_speed_rocket`, `round_speed_grenade` | true, 16, 80 | explosive rounds fly (m/s) from Snake's muzzle (§9) |
| `round_speed_rocket_boost`, `round_rocket_boost_after` | 60, 1 | a rocket's motor: from 16 to 60 m/s over 0.3 s after 1 s (docs/HOST.md "Rocket motor") |
| `flame_fx`, `flame_per_second`, `flame_speed` | true, 6, 14 | D-Walker's flamethrower drawn with Elden Ring flames |
| `[Puppet] motion`, `catch_up`, `unstick`, `unstick_seconds` | kinematic, 0.6, true, 0.5 | docs/HOST.md "Motion" |
| `[Stealth] box_hide`, `speffect`, `still_speed`, `slow_speed`, `slow_grace`, `reveal_distance` | true, 20004999, 0.4, 1.8, 1.5, 2 | the cardboard box (docs/HOST.md) |
| `[Stealth] poster`, `poster_range`, `poster_stare`, `poster_cooldown` | true, 15, 4, 12 | enemies stop and stare at a poster box |

### Guest: `PhantomRingGuest.ini [Combat]`

| Key | Default | Meaning |
|---|---|---|
| `LifeScale` | 4 | MGSV life per ER damage point |
| `NativeLifeWrite` | true | life written to `vars.playerLife` |
| `HurtFeedback` | true | `Player.RequestToSetCameraNoise{levelX, levelY, time, decayRate}` per hit (keys read from the exe at `0x1409DAE50`). MGSV's camera is ER's camera, so the shake shows in Elden Ring. Level 0.15–1 and time 0.25–1.2 s grow with the share of life lost. |
| `HurtBlood` | true | `Player.SetBloodToModel{pos = Vector3, radius}` (`0x1409DD4E0`) at Snake's chest on the side the hit came from. It shows on the captured model once rendering picks it up. |
| `NativeShots`, `FireFromAmmo`, `Melee`, `TranqWeapons`, `StunWeapons` | — | unchanged (GUEST.md) |
| `Blasts` | true | Snake's thrown and placed explosives as `kGEvExplosion` (GUEST.md 18.5) |

**Explosions: one route per explosive (guest, protocol v4).**

- **Thrown / placed** (hand grenades, sleep / stun / smoke grenades, molotovs, C4, claymores, mines): `kGEvExplosion`
  from MGSV's attack-area system, `v0` = the blast centre in MGSV world (where it went off in MGSV's flat sandbox: map it
  through the anchor like Snake), `b` = MGSV's own radius in mm (hand grenade 7000, sleep gas 9000, smoke 8000),
  `a` = `AmmoKind`:
  - `kAmmoLethal`: one event per blast;
  - `kAmmoTranq`: the sleep **gas cloud**: one event at impact and one every **0.5 s while the cloud lasts (~12 s, ~24
    events)**: apply a small sleep build-up per event to everything inside (the host's `Blast` gives `tranq_sleep / 4`);
  - `kAmmoStun`: stun grenades / net mines: stagger, no real damage;
  - `kAmmoSmoke`: smoke and flares, every 0.5 s for up to a minute: **no damage** (the host ignores them).
  `pad[0]` = MGSV attack id | class << 16 | owner << 24 (log it).
- **Gun-fired explosive rounds** (ZORN-KP grenades, rockets, under-barrel and grenade launchers, missiles): only the
  `kGEvFire` with the explosive bit in `pad[0]`; blast where the round hits in Elden Ring's world. MGSV's own blast of
  that round (e.g. `ATK_10603` for the ZORN-KP, 37 m away on MGSV's apron) is never sent: no double count (VERIFIED live).

**Damage share (guest, protocol v8, `kGEvFire pad[1]`; VERIFIED live 2026-10-12).** Every round Snake fires carries MGSV's own
lethal damage of one body hit as a share of a standard MGSV soldier's life, in 1/10000 (10000 = one hit kills; more = more than
one soldier's life; 0 = nothing lethal or unknown: use the per-class table). Shotguns: per pellet (`b` pellets each carry it);
explosive rounds: the round's own attack (the blast it makes in MGSV has the same attack id).

- Where it comes from (`mgsv/Combat.cpp`, `Addresses.h` "damage parameters"/"soldier parameters", byte-checked at install):
  the WeaponState's u16 at `+0x224` is the round's attack id (`TppDamage.ATK_*`); MGSV's damage table (`TppDamage.
  ReloadDamageParameter`, 489 rows of 26 bytes at `0x142A1A130 + 8`) gives its lethal (life) damage in row u16 [0], the
  non-lethal (stun/sleep) damage in [1] and the damage source (weapon class) at +0x16; the soldier's life is the
  `lifeParameterTable` maxLife of `TppSoldier2.ReloadSoldier2ParameterTables` (live: **2600**; stamina 3000, limbs 1500,
  armour 7500, helmet 500). Share = round(lethal x 10000 / 2600).
- Which column is lethal is read off the table (UNVERIFIED by code: its consumers go through the hub's service table): tranquilizer
  and stun rows have 0 there and their damage in [1]; lethal rows scale with the weapon class; grenades are above a soldier's life.
- Headshots, limbs and armour are not in the share (MGSV handles them with other columns and the soldier's limb/helmet/armour life):
  the host keeps its own headshot rule.

| Weapon (equip) | bullet | attack id | lethal / non-lethal | class | pad[1] | MGSV: body hits to kill |
|---|---|---|---|---|---|---|
| WU S.PISTOL tranquilizer (7, the new default handgun) | 10 BL_HgTranq | 325 | 0 / 750 (sleep) | 1 handgun | 0 | never (sleep) |
| S1000 AIR-S stun shotgun (871, the save's primary) | 22 BL_12GAAir | 383 ATK_40126 | 0 / 3000 (stun = a soldier's whole stamina) | 4 shotgun | 0 | never (knocks out in 1) |
| ZORN-KP grenade pistol (875) | 23 BL_HgGrenade (explosive) | 347 | 2600 / 0 | 5 grenade launcher | 10000 | 1 |
| CGM 25 rocket launcher (873) | 59 (explosive) | 443 | 2200 / 0 | 8 missile | 8462 | 2 |
| EQP_WP_West_ms_010 FB MR R-LAUNCHER (54, the default `Primary2`) | 46 (explosive) | 432 | 3500 / 0 | 8 missile | 13462 | 1 |
| EQP_WP_East_ar_010 assault rifle (36) | 15 | 364 ATK_30001 | 550 / 0 | 3 assault rifle | 2115 | 5 |
| EQP_WP_West_sr_010 sniper rifle (41) | 37 | 412 | 1850 / 0 | 6 sniper | 7115 | 2 |
| D-Walker's flamethrower | (no rounds) | ATK_WalkerGear_Flame 150 | -- | -- | (kGEvBuddyFire pad[1] = reach) | -- |

  The per-weapon rows of the table (attack ids 319-488) by damage source, lethal per hit (-> share): 1 handgun 400-1900
  (1538-7308), 2 SMG 400-500, 3 assault rifle 400-680 (1538-2615), 4 shotgun 450-3100 per pellet, 5 grenade launcher 2600
  (10000), 6 sniper 1200-2100 (4615-8077), 7 MG 550-680, 8 missile 2100-4000 (8077-15385), 9 (heavy) 2600-4000, 10 grenade
  4500-5600; non-lethal rows 0 lethal and 400-3500 stun/sleep in [1] (the sleep and stun rounds: pad[1] = 0, the ammo kind tells
  the host). **Player report "the gun does very little
  damage" (session 4):** the gun was the save's S1000 AIR-S, a *stun* shotgun (no lethal damage in MGSV at all; it knocks soldiers
  out); the 13-per-hit numbers in that log were D-Walker's flamethrower.

**Rocket flight (guest, for the host's own copy):** MGSV's CGM 25 round (bullet 59) fired level along MGSV +x set off its blasts
1.16 s after the shot 19 m on, then along its line to 150 m by 3.5 s (two shots, identical): flight time ~0.8 s + distance / 60 m/s.
The guest still sends only the `kGEvFire` at the moment of the shot (no rocket position: MGSV's projectile isn't tracked); a host that
wants a visible flight can delay the impact by that much (`rocket_shots.prrec`).

## 4. Dev console

```
pr er combat                         route + verification state, ER-dealt %, counters, param tables
pr er "combat targets"               the 16 nearest characters: npc, team, HOSTILE/friendly, hp, capsule,
                                     and (puppet on) the yaw/pitch to aim fake_mgsv's !fire at them
pr er "combat hit <n> [dmg [poise]]" a hit on character n through the current route (no aiming needed)
pr er "combat sleep <n> [power]"     sleep build-up on character n
pr er "combat team <n>"              team of n, the Tarnished's, and whether ER calls it hostile
pr er "combat resist <n>"            n's resistance module: what is left before each status procs, the thresholds
pr er "combat effects <n>"           n's active SpEffect ids;  "combat speffect <n> <id>": ApplySpEffect directly (diagnostics)
pr er "combat hit|sleep <n> ... above"   the bullet comes down on top of n (a raised shield can't take it)
pr er "combat set tranq_share|tranq_share_head|gas_share|choke_share|sleep_correction <v>"   live tuning
pr er "combat route auto|bullet|hp|ini"
pr er "combat damage_route [topup|speffect|ini]"   the damage route (§2 "Damage that syncs"): ladder rows written?, confirmed?,
                                     rows arrived / missing, guarded, topped up, % of the wanted damage dealt, the guard rule
pr er "boom lethal|stun|sleep|smoke [radius]"   an explosion visual 5 m in front of the Tarnished (§9); `boom` = status
pr er "boom mode auto|bullet|sfx|off|ini"       `boom restore` puts the carrier rows back now
pr er status                         the puppet line includes host falls and "DYING for Snake"
pr er "progression [off|level|weapon|both]"   the Tarnished's level, attributes, right-hand armament and its upgrade, the factors
```

fake_mgsv (`pr fake-mgsv`, guest console `pr lua '!…'`): `!walk on|off`, `!autofire on|off`,
`!fire <lethal|handgun|rifle|sniper|buck|tranq|stun|explosive> [n] [yaw_deg] [pitch_deg]`,
`!cqc <punch|kick|throw|choke>`, `!blast [radius]`, `!die`.

## 5. Files

- Explosion visuals (§9): `er/ExplosionLogic.{h,cpp}` (pure), `er/Explosion.{h,cpp}`, `er/BulletSpawn.h` (SpawnBullet's
  argument block, shared with `er/Combat.cpp`).
- Thrown / placed explosives (v5/v6): `er/ThrowLogic.{h,cpp}` (pure: flight, bounce, roll, the aiming arc),
  `er/Throw.{h,cpp}`, `er/Mines.{h,cpp}`, `er/RenderMarkers.{h,cpp}` (+ the marker pipeline in `er/Renderer.cpp`,
  shaders in `er/Shaders.cpp`), `er/WorldShift.{h,cpp}`; tests `tests/test_host_throw.inc` (in test_host_combat).
- Host: `er/CombatLogic.{h,cpp}` (pure), `er/Params.{h,cpp}`, `er/Combat.{h,cpp}`, `er/Events.cpp`
  (dispatch), `er/Puppet.cpp` (death/respawn, fall damage, patch restore), `er/HitHook.*` (unchanged
  logic, on by default), `er/Config.*`, `er/PhantomRingHost.ini`.
- Shared, a few lines: `er/Addresses.*` (12 entries), `er/Console.cpp` (`combat`),
  `protocol/phantomring_protocol.h` (comments documenting `GuestEvent.pad[0]` for Fire/Melee; no
  layout change, still v2), `CMakeLists.txt` (`test_host_combat` in `check_host`).
- Guest: `mgsv/Logic.*` (`FeedbackFor`, `KindOfBullet` moved here), `mgsv/Combat.*`, `mgsv/Frame.cpp`
  (queues `hurt`), `mgsv/GuestScript.cpp` (`PR.Hurt`), `mgsv/Config.*`, `mgsv/PhantomRingGuest.ini`.
- Tests: `tests/test_host_combat.cpp` (+ the real-regulation checks when `build/er_params` exists),
  `tests/test_guest_logic.cpp`, `tests/test_guest_script.lua`, `tests/test_guest_link.cpp`.
- Tools: `tools/re/er_params.py` (decrypts `regulation.bin`, dumps/greps params with fromsoftware-rs's
  layouts; `ids Bullet` once fills `build/er_params` for the test), `tools/phantomring/fake_mgsv.py`.

## 6. LIVE TEST (for the lead)

Build (`cmake --build build`, under the build lock) and run `check_host`/`check_guest`. To also run the
real-data checks: `python3 -I tools/re/er_params.py ids Bullet`, which writes `build/er_params`. Then `pr
launch` copies the new DLLs and ini as usual. Keep `[Debug] debug = true` in the host ini for the
first run (bullet-landed lines and hit-hook lines are debug after the first 120 Info lines). Stand the
saved character somewhere with weak enemies in reach: Limgrave soldiers, wandering nobles, or a camp.

### Stage A: Elden Ring only, with fake_mgsv (no MGSV needed)

1. `tools/pr launch --er-only` and get past the title (HOST title automation). Then run `tools/pr fake-mgsv`
   in a second terminal. Wait for `puppet ON`. Run `pr lua '!walk off'` and `pr lua '!autofire off'`
   (Snake stands still; the puppet keeps the Tarnished's facing).
2. `pr er "combat targets"`. Enemies show as `HOSTILE` with a capsule of radius ~0.4–0.7 and height
   ~1.4–2.4 `(hit)`. Merchants/spirit ashes show `friendly`. If every capsule says `fallback`, the
   physics offsets are wrong (check `peek [[[chr]+0x190]+0x68]+0x2F0 f32 2`).
3. `pr er "combat hit 0 60 30"` on a hostile one. Expect, within a second:
   - H-log `params: Bullet = holder 10 (name 'Bullet', 15475 rows ...)` and the same for AtkParam_Pc
     (holder 8) and SpEffectParam (15);
   - `combat: console hit -> npc … : bullet 60 poise 30 lv 2`;
   - **the enemy flinches and turns on the Tarnished**;
   - `combat: bullet on … landed: hp X -> Y` and `combat: bullet route CONFIRMED`.

   `pr er combat` then shows `ER dealt N% of the wanted damage`. Set `bullet_damage_mult ≈ 100/N`
   (ER's defence; the formula already scales by enemy HP). Failure cases:
   - `SpawnBullet(…) refused (error 3/4)`: try another `bullet_id`;
   - `did not land … hp fallback`, 3 times, then `switching to the hp route`: the bullet spawned but didn't
     hit. Try a larger `bullet_offset` (1.2), or `combat route hp` to keep playing. Record the case.
4. `pr er "combat sleep 0 250 above"` (the puppet must be on: the bullet row is patched): `combat resist 0` shows the sleep
   entry fall and reset to a higher threshold, and the enemy lies down with Elden Ring's sleep sparkle. `SpEffectParam
   25019 is not a sleep build-up row` means the regulation changed.
5. Aimed shots: `pr er "combat targets"` prints `aim <yaw> <pitch>` for each one. Run
   `pr lua '!fire rifle 1 <yaw> <pitch>'`. Expect a red tracer to the enemy and
   `Snake rifle (weapon 291 bullet 15 x1) -> npc …`, then the hit lines. Then try `!fire tranq` (sleep
   only, no aggro), `!fire stun` (big stagger), `!fire buck` (one hit with 8× damage),
   `!fire explosive` (blast: every hostile within 4 m), `!cqc kick` / `!cqc throw` (enemy within 1.6 m in
   front) and `!blast 6`. A friendly NPC must never be hurt (`not hostile … no damage` in the debug log).
6. ER hits the puppet: let an enemy hit it. Expect H-log
   `hit hook: CSPlayerDamageModule … slot 7 -> PhantomRing` at puppet start, no stagger or HP loss on the
   Tarnished from the hit, debug `puppet: hit hook N HP … from D m`, and fake_mgsv's log `hurt` counting
   up with the HP bar following its life. A crash or a frozen enemy on the first hit means
   `hit_hook = false`; report it.
7. Death: `pr lua '!die'`. Expect `guest: Snake died`, then `puppet: killed the ER player (ChrIns::Kill)`,
   YOU DIED, the grace respawn, `ER player respawned`, and fake life back to 6000. If the player doesn't die:
   `still alive … retrying` ×2, then `would not kill the player … refilling Snake`. Report it.
8. Fall: `pr er "call setpos <x> <y+25> <z>"` (puppet position from `pr er status`). The puppet re-anchors
   and falls. Expect `puppet: landed at ~22 m/s -> fall damage 100% … lethal`, then the death sequence of
   step 7. A 10 m drop (`y+10`) should cost ~20%, 15 m ~65%.

### Stage B: both games (real MGSV)

1. `tools/pr launch`; wait for `host status: ready`, `puppet ON`.
2. Equip a handgun / AR in MGSV and shoot a Limgrave soldier. You should see tracers, `Snake handgun
   (weapon N bullet 5 …) -> npc …`, flinches, and death after ~4 body shots or ~2 headshots. If
   `bullet 0` shows, the ShotSolver hook isn't on: it's the ammo-counter fallback with default damage.
3. Tranquilizer pistol (WU S.Pistol, bullet 10/11): the enemy falls asleep (head faster) and does not aggro.
4. CQC: `Snake CQC kind 1 (MGSV attack 7)` for kicks. Note the raw attack ids of punches, throws and
   chokes from the log so `mgsv/Combat.cpp` can map them (only 7 = kick is known; everything else is
   sent as a punch).
5. Get hit: Snake's life drops (`pr lua 'return vars.playerLife'`), MGSV's camera shakes (it is ER's
   camera too), and the guest log (with `[Guest] Debug = true`) shows `lua: hurt … -> hurt=noise true blood true`. Let him die: YOU DIED,
   respawn, life refilled.

Report back: the ER-dealt percentage, any bullet route failure, the hit-hook behaviour, and the CQC
attack ids.

## 7. Known limits / next steps

- One AtkParam row serves all in-flight bullets (last write wins within a frame).
- Damage: MGSV's own share per round when the guest sends it (protocol v8), else the class table (tuned by feel: MGSV's
  per-weapon numbers live in Lua equip tables we don't read). The top-up (§2) makes it the damage dealt; ER's own
  weapon-based hit is only the reaction. Bosses are recognised by max HP against the Tarnished's, not by the boss bar.
- The top-up lands on the frame after Elden Ring's own hit (31-49 ms after the shot, VERIFIED live; it used to wait
  `verify_ms`, 300 ms). Applying it inside the enemy's ReceiveDamage would save one more frame but needs a hook on every
  enemy's damage module; not worth it at 2-3 frames.
- Guard detection is a heuristic (ER's damage of one of our bullets vs the best seen on that kind of enemy).
- CQC: punches (80) and the kick (83) are identified; throws and chokes need a live target in MGSV (MGSV runs
  them only on its own soldiers, and the sandbox has none): Snake's CQC in Elden Ring is strikes only.
- Thrown and placed explosives (protocol v5/v6): the host flies its own copy of each throw through Elden Ring's world and
  keeps placed ones where they were put; MGSV's `kGEvExplosion` (pad[1] = the id) is put where our copy is (HOST.md "Thrown
  and placed explosives"); without an id, as before at MGSV's point through the anchor. `Blast()`: smoke nothing, sleep
  build-up (`gas_share`), stun stagger (non-lethal), lethal falloff. Grenade rounds from guns stay kGEvFire.
- Tranquilizer: kGEvFire with `kAmmoTranq` (the BURKOV TB, GUEST.md 18.7). VERIFIED live with fake_mgsv's `!fire tranq` at a
  Gatefront soldier 18 m away: dart 1 `sleep +136 (resist 226)` (left 226 -> 90), dart 2 -> asleep (threshold reset to
  268), HP 219 throughout. And with both games (2026-10-12 afternoon): a BURKOV TB from the iDroid's supply drop, two darts
  in a torch-bearing Limgrave soldier 16.6 m away (`Snake tranquilizer (weapon 875 bullet 10 x1) -> npc 43005210 ...: sleep
  +101 (resist 169)` twice): asleep (sleep gauge `+0x64` 1, threshold 169 -> 197), HP 96/96.
- Hits on characters ER's world ray passes through (no collision) still count, through our capsules;
  walls stop shots. Foliage stops them too when ER's ray hits it (a soldier behind a bush: "-> world").
- The ActorProxy model (DESIGN §8.1 "later") would replace all of this with MGSV's own ballistics.

## 8. Verified live, both games (2026-10-12, Gatefront / Agheel Lake, Limgrave)

| What | Result |
|---|---|
| ZORN-KP grenade round (bullet 23, explosive) at a 219 HP soldier | ER 42-63 HP, +313 topped up: dead in one round, **+64 runes**; flinch/stagger from 80 poise |
| the same, before the top-up | ER ~42 per round whatever we asked (38/190/714 attack), 5 through a raised shield: 6-8 rounds per soldier |
| S1000 AIR-S stun round (bullet 22) | ER 1 HP (correction cut to 15%) + 4 topped up = 5, poise 40, dmgLevel 3: big stagger, no kill |
| CQC (RT unaimed next to a soldier) | `Snake CQC kind 0 (MGSV attack 80)` -> bullet 27, landed 146 -> 120 |
| D-Walker flamethrower, 3 s on a soldier | 22 flame events, ER 1-2 + 11-12 topped up each (fire): 133 -> 42 HP, ER's burning VFX on him |
| a soldier's hits on Snake | 43-81 HP on a 522 HP Tarnished -> kHEvHurt 161-303 -> Snake 7801 -> 6648 (MGSV regenerates ~760 life/s after a moment) |
| Snake killed by a group | `Snake died` -> ChrIns::Kill -> YOU DIED; Snake dives and lies down in MGSV, drawn lying there in ER (`death_visual = keep`); respawn after the load, life 7801/7801 |
| a mob near a Stake of Marika | "Choose revival location" waits for the player (dead bit already clear): Snake stays down until the respawn load |
| bullet route | confirmed on the first hit every session; 0 hp fallbacks |

**Margit, the Fell Omen** (2026-10-13, Elden Ring with fake MGSV, the player's level-15 save, 572 HP; `combat shoot rifle`
at his chest with MGSV's assault-rifle share 0.2115, one round per 100 ms):

| What | Result |
|---|---|
| one 30-round magazine, `boss_lives` 70 | 4174 -> 3764: 410 HP = **9.8%** (13-14 a round) |
| the same, `boss_lives` 45 (now the default) | 4174 -> 3567: 607 HP = **14.5%** (~20 a round) |
| Margit's hits on Snake | 143-234 HP on the hit hook -> kHEvHurt 375-614 (25-41% of Snake's life); one combo took 523 of 572 as the Tarnished |
| Snake killed by him | `Snake died` -> ChrIns::Kill -> YOU DIED -> respawn at the Castleward Tunnel grace (closer than the Stake: no choice offered) 18.9 s later, `kHEvRespawn` + `kHEvRested 1`, Snake back; the walk to the fog and through it again (the step-aside) worked twice |

Margit was never brought below 85%.

**With the real MGSV** (2026-10-13, combined run, the player's save; Snake aims with LT and the right stick, RT fires the
SVG-76 for real, the camera turned onto Margit's chest every 80 ms):

| What | Result |
|---|---|
| 26 rounds (from 30/210 to 4/184) | 4174 -> 3644: 530 HP = **12.7%** (the bullet route confirmed: 25 landed / 0 missed, ER dealt 54% of the wanted damage, 24 topped up) |
| a full magazine (30 rounds, auto-reload after) | 4174 -> 3574: 600 HP = **14.4%** |
| Margit's hits on Snake, 29 hits in two fights | 11-45% of Snake's 7801 life each (11, 15, 21, 25, 29, 33, 41, 45%); MGSV regenerates ~760 life/s right after each hit (it never saw one), so Snake survived 18 hits in 70 s standing next to him, and died when hits chained (twice) |
| death | `Snake died` -> YOU DIED -> the Castleward Tunnel grace 18.6 s later, `kHEvRespawn, kHEvRested 1`, guest `ammo: refill (respawn: magazines 36:4->30; stocks 184->210)`, life 7801; the fog again, through, the fight again |
| sleep grenade at his feet | thrown (LT, then RT while LT is held), 2 bounces, MGSV's fuse 4.97 s, Elden Ring's sleep mist r 9 m (whether Margit's sleep build-up rose was not read) |

Margit was never brought below 85% (lowest 3574 of 4174).

## 9. Explosion visuals (Elden Ring effects at MGSV's blasts)

Player report: "the rocket launcher made sounds and seemed to do damage but no explosion". Elden Ring draws only Snake's and
D-Walker's meshes; MGSV's effects never reach it. Every blast the host already handles (§3: explosive rounds' impact
points, `kGEvExplosion`) now also gets an Elden Ring-native effect at the Elden Ring point (`er/Explosion.cpp`,
`er/ExplosionLogic.cpp`). Damage is unchanged: `Blast()` stays the only source of it.

| MGSV | look (BulletParam burst row, effect) | per burst | repeats |
|---|---|---|---|
| lethal: grenades, C4, mines, rockets, grenade rounds (`explosion_radius` for rounds) | Fire Pot burst 10030001, fireball 300122 + its 18 sparks and ground flames (10030002/3, no attack: kept) | 1.6 m | no |
| stun grenades | Holy Water Pot burst 10035001, golden flash 300182 | 1.4 m | no |
| sleep gas (~12 s cloud, a report every 0.5 s) | Sleep Pot mist 10064001, effect 300212, 3 s, grows 1 -> 4 m | 4 m | one per `explosion_cloud_repeat` (2.5 s) while MGSV reports it |
| smoke / flares (up to a minute) | Fetid Pot cloud row 10033001 with the Fetid Pot's own cloud effect 300192 (normally its hit effect at the same point) | 1.4 m | same |

A blast bigger than 1.25 bursts gets a ring around the centre (at most `explosion_max_bursts` = 7 bursts; ring pulled in
until the bursts touch): a 4 m rocket is a centre + 6 fireballs within ~2.5 m, MGSV's 9 m sleep gas a centre + 6 mists
6 m out. Points that come from MGSV's world (thrown explosives, clouds) and every ring burst are dropped onto Elden
Ring's ground with a short ray (`explosion_ground`); explosive rounds' impact points are already on a surface.

**How (route `bullet`, research 2026-10-12, offline).** Elden Ring's pots are bullets: the flying pot (Fire Pot 10030000)
hits and spawns its burst bullet (10030001: life 0.07 s, hit radius 1.6 m, atk 10030001 = 230 fire), and the burst's
*flight* effect is the fireball, which outlives the bullet (`bullet_sfx_delete_type_by_life_dead` 0 on every pot burst).
So spawning a burst row directly at a point plays the pot's explosion there. What hurts is its attack (`atk_id`) and its
SpEffects (on hit `spEffectId0..4`, on the owner `spEffectIdForShooter`): the copy we spawn has atk 0 and SpEffect 0, the
way Elden Ring writes its own effect-only rows (the Sleep Pot's mist 10064001 has atk 0; its only effect is SpEffect
500640, the sleep build-up, which the copy drops). Children are kept only if none of them has an attack or SpEffect (the
Fire Pot's sparks and flames: kept; the Lightning Pot's bolts: cut). `createLimitGroupId` is cleared (our own caps apply).

The copies live in **carrier rows nothing spawns**: the cut Roped Freezing Pot's (goods 480, `[ERROR]` name: 10048000,
10048001) and Miranda's Prayer's (goods 3020: 10302000, 10302001) bullets. Across all 183 extracted params, the only
i32 equal to a carrier id is in those two goods rows and in those bullets' own chains (`test_host_combat` checks it; an
event script, EMEVD, could still name a bullet id: not checked). So real pots, the throwing-dagger row the hits use, and
everything the player or enemies throw stay exactly as shipped. The carriers are written once per look (`params::Patch`,
the same undo as §2) and go back when the puppet is off and the last burst has had `life + 8 s` to play out (a 3 s mist
still reads its row), or with `boom restore`.

Spawned with `CSBulletManager::SpawnBullet` (0x3A2CB0, verified on every start, live-proven by the hit route), owned by
the Tarnished, no target (-1), level, facing away from him. No new address. Caps: `explosion_per_second` (40) bursts per
second, clouds gated per spot and kind.

**`sfx`**: a direct effect spawn (CSSfxImp / GXFfxSceneCtrl). fromsoftware-rs has the structures (`cs/sfx.rs`,
`debug_spawn_ffx_id`) but no spawn function, the CT has none either: nothing verified to call, so `sfx` logs why and
draws nothing. `auto` = `bullet` when SpawnBullet, CSBulletManager and SoloParamRepository verify, else off (logged once
per reason, e.g. "CSBulletManager not created yet" while loading).

**Verified offline**: rows, fields and chains against this `regulation.bin` (`test_host_combat`: each look is harmful as
shipped, its copy harmless with the expected effect, the fire chain is the 2 harmless rows, the carriers unreferenced),
the pure rules (kinds, ini parsing, layout, cloud gate, carrier building). **UNVERIFIED (needs a live run)**: that a burst
row spawned directly shows its effect and plays its sound (the sound is presumed inside the effect: the burst rows'
`seId*` fields are all 0); that the Fetid Pot's hit effect shows as the smoke carrier's flight effect (if not:
`explosion_smoke = 10037001`, the Poison Pot cloud, or `10064001`); that atk 0 bullets neither flinch nor aggro anyone;
how big each look reads next to MGSV's radius.

### Verified live (2026-10-12, Gatefront, ER 1.17.1)

- `boom lethal|stun|sleep|smoke` (radius 4): each look prepared (`explosion: lethal = Bullet 10030001 (effect 300122, 1.6 m
  per burst, 0.07 s, its 2 harmless child rows kept) as carrier Bullet 10048000`, likewise 10035001/300182 1.4 m, 10064001/
  300212 4 m 3 s, 10033001 + 300192 1.4 m 0.6 s) and drawn (`hud shot`): a Fire Pot fireball cluster, the Holy Water Pot's
  golden flash, the Sleep Pot's pale purple mist, a brown-orange Fetid Pot cloud (the smoke look works; it reads more like
  dust than MGSV's white smoke). No `SpawnBullet(...) refused`.
- Harmless: 24 sleep-gas reports with 7 mist carriers bursting over a soldier (and a stun flash, a frag cluster nearby):
  HP 219 -> 219 apart from our own hits; the carriers carry no attack.
- Thrown: a real MGSV hand grenade's blast as a fireball cluster where our copy lay on the hillside; sleep gas as a ~9 m mist
  cloud (7 mists) renewed every 2.5 s for MGSV's ~12 s; mines' blasts where they lie (HOST.md).
- Carriers go back: `explosion: puppet stopped; carrier rows go back 11 s after the last burst` ... `carrier Bullet rows
  restored (puppet off)`; `boom` -> `carrier rows: original`.
- Sizes: a lethal blast of MGSV's 7 m (hand grenade) is a centre + 6 fireballs within ~2.5 m -- about the visible size of
  MGSV's own fireball (its 7 m is the damage radius, applied by `Blast()`); sleep gas 9 m a centre + 6 mists 6 m out (the
  whole cloud); kept.

### D-Walker's flames, breakers, rounds in flight, the pool (2026-10-12 evening)

- **Flames** (`explosion_flame` 10700000, carrier 10302002): `kGEvBuddyFire` with no bullet (pad[0] = 0) now draws one
  Dragonfire flame flying `flame_speed` 14 m/s from D-Walker's muzzle along the stream (`flame_per_second` 6). The breath
  row is an emitter (`shoot_interval` 0.2: live, one spawn went on shooting for minutes), so the carrier is a single shot
  (`explosion::MakeSingleShot`: num_shoot 1, no interval) and every spawn is one-shot (below). Damage stays the flame
  class's (fire). VERIFIED live: a stream of fire from D-Walker to the wall (screenshot), also with `kGuestFirstPerson` set.
- **Breakers and shrapnel** (`explosion_breaker` / `explosion_shrapnel`, carriers 10302003/4): object-only bullets that
  break Elden Ring's destructible assets where a round hits the world, a CQC strike meets no one, or a lethal blast goes
  off (docs/HOST.md "Breakables", with the live results: carts and crates break, a defence-90 fence did not).
- **Explosive rounds fly** (`round_flight`): a rocket at 20 m/s, a grenade round at 80 m/s, drawn with a trail, the blast
  where they hit (docs/HOST.md "Explosive rounds in flight"; live: 17.3 m in 0.87 s, 53.2 m in 2.66 s).
- **The pool**: every `SpawnBullet` now marks its emitter one-shot (spawn data +0x44 bit 0); before, emitters piled up
  until Elden Ring refused every spawn (`error 4`), hit bullets included. Ours alive are capped by `explosion_max_live`
  (48) and by Elden Ring's own emitter (192) and bullet (176) counts; clouds last `explosion_cloud_life` (6 s). `boom pool`.
  Live: 250 blasts in a row capped without a refusal, emitters 14 -> 7 -> 0 after.

### Live test

Stage A (Elden Ring alone is enough; `boom` needs no puppet), standing in an open, quiet spot (no NPCs you care about
within 15 m; try once near an enemy camp too). In Elden Ring's console (`tools/pr er ...`):

1. `pr er boom` -> `explosion_fx auto -> bullet`, each look "not prepared yet", carrier rows original.
2. `pr er "boom lethal"` -> a Fire Pot fireball 5 m ahead, with its sound, plus a ring of 6 (radius 4). H-log:
   `explosion: lethal = Bullet 10030001 (effect 300122, 1.6 m per burst, 0.07 s, its 2 harmless child rows kept) as
   carrier Bullet 10048000 (no attack, no SpEffect)` and `explosion: console boom -> lethal r 4.0 m ...: 7 burst(s)`.
   `pr er "boom lethal 1"`: one fireball. Walk into the next one: no HP loss, no burning.
3. `pr er "boom stun"`: a golden flash; `pr er "boom sleep"`: a pale mist for ~3 s (stand in it: no sleep bar);
   `pr er "boom smoke"`: a brownish cloud. If smoke shows nothing: `boom mode off`, set `explosion_smoke = 10037001`
   in the ini, restart, retry, and report which one looked right.
4. Next to a hostile enemy: `pr er "boom lethal 2"` at it: it must not flinch, lose HP or aggro (`pr er "combat
   targets"` shows its HP unchanged).
5. Throw a real Fire Pot as the Tarnished (puppet off): normal damage (the template rows are never touched).
6. `pr er boom` -> carrier rows `patched`; ~10 s with the puppet off -> H-log `explosion: carrier Bullet rows restored
   (puppet off)`.

Stage B (both games): rocket launcher / grenade launcher at the ground and at an enemy: one cluster per impact
(`explosion: Snake explosive (...) -> lethal r 4.0 m`), damage lines as before. Hand grenade: fireballs where it lands
in Elden Ring (on the ground, not floating). Sleep grenade: mist renewed every 2.5 s for ~12 s (`explosion: explosion
kind 1 -> sleep ...` about 5 times; `pr er boom` counts the skipped repeats), enemies inside still fall asleep (our sleep
build-up). Smoke grenade: clouds for as long as MGSV's smoke lasts. Report: what each looked and sounded like, size vs
MGSV's blast, any `SpawnBullet(...) refused` line, frame-rate dips during a big cloud.

## 10. Live test: damage that syncs (`damage_route = speffect`, roadmap §4 C1)

Everything in §2 "Damage that syncs" is UNVERIFIED live. Offline: the ladder's row selection and rounding, the split, the
guard rules and the kills per magazine are unit-tested (`test_host_combat`: `TestDamageLadder`, `TestNativeDamage`, with the
live numbers below as an Elden Ring model), and the 36 carrier rows and every SpEffectParam offset written are checked
against this regulation. No new game address. Single player first; co-op only once single player passes.

Setup: back up the save (`pr` does it on launch), stay offline. In the deployed `PhantomRingHost.ini` set `[Combat]
damage_route = speffect` (or leave the ini and switch live with `pr er "combat damage_route speffect"`) and `[Debug] debug =
true`. `tools/pr launch --er-only` + `tools/pr fake-mgsv` (Stage A of §6), `pr lua '!walk off'`, `pr lua '!autofire off'`.

**A. Do the rows land at all** (the question the route stands on). Stand near one Limgrave soldier (219 HP at Gatefront).
1. `pr er "combat damage_route"` -> `damage route speffect (...; ladder SpEffectParam 20010640..20010675 not written, unknown)`.
2. `pr er "combat targets"`, then `pr er "combat hit <n> 46 20"`. Expect H-log `combat: damage ladder ready -- SpEffectParam
   20010640..20010675 carry 1..9999 HP`, then `bullet 46 ... = ER ~0 (10%) + ladder 46 (2 rows)` and `bullet on ... landed ...:
   hp 219 -> ~169 (wanted -46) = ER ~4 (expected 0, correction 10%) + ladder 46 ARRIVED`, then `damage ladder CONFIRMED`. The
   soldier flinches and turns on the Tarnished.
   - If instead `ladder 46 MISSING` with ER's ~4 only: the first three such hits are topped up (`+42 topped up (ladder
     unconfirmed)`), then `the damage ladder's rows did not arrive on 3 hits in a row -- damage_route topup`. Record the lines,
     `pr er "combat effects <n>"` right after a hit (are 20010645/20010652 listed for a frame?), and stop: the carriers need
     another template (next try: copy all of 99441, i.e. also category_priority 101 and target bits 127/7).
3. Hit him again 4 more times: `= ER ~5 (expected 5, correction 12%) + ladder 41 ARRIVED`; dead on the 5th hit, **runes
   given** (note the rune count before/after: +64 at Gatefront with the top-up). If he dies but no runes: record it (the
   ladder's kill is not credited to the Tarnished) and switch back to topup.
4. `pr er combat` -> the second line `damage route speffect (ini|console; ... written, confirmed): N ladder hits, rows arrived
   N / missing 0, ... dealt ~100-105% of the wanted damage`.

**B. Same kills per magazine as §8** (the regression). With the real MGSV (or `combat shoot rifle x y z 0.2115` at the body):
1. A Limgrave soldier: 5 body hits / 1 headshot (`HEADSHOT ... bullet 230`: `ladder 2xx` in 3 rows), as with the top-up.
2. Margit (the player's save, fog wall at Stormveil): one 30-round assault-rifle magazine at his chest -> 14-15% (topup:
   600 HP = 14.4%, 4174 -> ~3574). Each line `= ER ~2 (...) + ladder ~18 ARRIVED`. Then `combat damage_route topup` and a second
   magazine for the comparison in the same fight.
3. D-Walker's flamethrower on a soldier for 3 s: each flame hit `+ ladder ~11 ARRIVED`, his HP 133 -> ~42 as in §8 (the
   burning VFX still from ER's fire part).
4. Stun round / sleep dart / sleep gas: unchanged (they keep the top-up: no `ladder` in their lines).

**C. Guard.** A shield soldier (Gatefront / the Stormhill shack) with his shield up, shot from the front 5 times in 2 s:
- the first blocked hit logs `GUARDED (12% through)` and then `damage ladder on a guarded hit: <rule>`. Record the rule:
  - `a guarded hit still delivers it`: the next blocked hits carry `shield up: x0.12` and deal ~5-10 HP each (topup: 5 + 40);
    after he lowers the shield, full hits again;
  - `Elden Ring drops it on a guarded hit`: blocked hits deal ER's ~1 only (a shield blocks Snake completely: playable?
    compare with the topup's 12% through and say which feels right).
- either way: no `MISSING` counted as a route failure (the ladder stays confirmed).

**D. Restore.** Stop the puppet (`pr er "puppet off"` or the toggle): H-log `combat: bullet/AtkParam rows restored`; as the
Tarnished hit an enemy with a Throwing Dagger (Bullet 10170000) and a bleed weapon: normal damage, no stray ladder damage.

**E. Co-op (only after A-D pass; Seamless, both players on this build and the same ini).** The client (not the session
host) shoots the host's enemy: both screens show the same HP after each hit, the enemy dies on both, both get runes (C1's
check). Then the reverse (host shoots). With `damage_route = topup` for comparison: the client's top-up shows on the client
only (that is the bug this route fixes).

Report: the H-log lines of A.2, the rule of C, the Margit %, and the `combat` summary at the end.

### Live 2026-10-14 (single player, fake MGSV, Stormveil soldiers npc 30003214 / 30200014, 221 / 317 HP)

**A. The rows land -- a few frames late.** The first runs read `ladder 46 MISSING` on every hit and gave the ladder up after
three (`the damage ladder's rows did not arrive on 3 hits in a row -- damage_route topup`), each hit topped up. But each
target's HP before the next hit was another ~46 / ~41 lower than the hit's own line said (221 -> 220 + 45 topped up = 175,
then 129 at the next hit: the 46 of the ladder). Measured with the soldier's HP read every ~65 ms (`peek
[[chr+0x190]+0]+0x138 i32`): 221 at the hit, 220 at 131 ms (Elden Ring's own part, 1 HP), 129 at 199 ms (+45 topped up and the
ladder's 46). The rows arrive ~1-4 frames after Elden Ring's own hit; the check judged on the first HP drop (10-64 ms).
Both carrier forms delivered: the blank rows (default) and copies of Elden Ring's own "10 HP once" row 99441 (`[Combat]
speffect_template = 99441`, `combat set speffect_template 99441`; the HP fell by the ladder's amount the same way).
**Fix:** a ladder hit is judged once its damage showed, or `[Combat] speffect_wait_ms` (300) after Elden Ring's own part did.
With it (after a respawn, 3 hits on a fresh soldier): hit 1 waited 300 ms and still read `MISSING` (221 -> 178: 43, 3 short of the
ladder's 46 alone -- the check wants all of it), +3 topped up; hit 2 `= ER 1 (expected 4) + ladder 42
ARRIVED` -> `damage ladder CONFIRMED`; it was also read as GUARDED (ER's part 1 of an expected 4: the guard rule's 4 HP floor
is too low for these ~1 HP parts) -> `a guarded hit still delivers it`, so hit 3 carried only `ladder 10 (shield up: x0.23)`
and the soldier still lost 62 (ER 52 by the split: another source in that window, UNVERIFIED which). Route summary: 3 ladder
hits, rows arrived 2 / missing 1, dealt 107% of the wanted damage.
**B. Kills, the same as the top-up** (a fresh launch, the Tarnished unhittable with `flag nohit on`, assault-rifle shots
`combat shoot rifle x y z 0.2115` at a soldier's chest, 0.8 s apart): `topup` -- 5 hits of 47 (221 -> 174 -> 127 -> 80 -> 33 ->
0); `speffect` -- the first hit `= ER 1 + ladder 47 ARRIVED` -> `damage ladder CONFIRMED`, then `= ER 5 (expected 5, correction
47%) + ladder 42 ARRIVED` on each, 47 a hit, dead on the 5th landed hit (its ladder `MISSING` there: Elden Ring's own 8 HP part
had already killed him -- not counted as a failure). **Rapid fire** (4 shots 67 ms apart, inside each other's 300 ms wait):
221 -> 34, 187 of the wanted 188. Not done: Margit, a real shield (C), D-Walker's flame, D/E. **Verdict:** in single player the
ladder now deals what the top-up deals, hit for hit; the earlier noise (a first hit read short, then a false "guarded") came
from judging before the rows arrived. **`topup` stays the default** all the same: the ladder's only gain is co-op sync (not
played yet), and a boss, a shield and the flame are untested; `pr er "combat damage_route speffect"` (or the ini) to play it.
Next: the guard rule should not judge Elden Ring parts of a few HP, and the arrival test could accept a ladder a few HP short.
