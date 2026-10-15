#pragma once
// PhantomRingGuest.ini, parsed once at activation. Every key is documented in PhantomRingGuest.ini.

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::guest
{
	enum class HoldMode
	{
		kOff,              // never move Snake back (he walks off the sandbox eventually; debugging only)
		kTreadmill,        // past the radius: Lua Warp back to the centre (proven script path), jump left out of the odometer
		kNativeTreadmill,  // the same, through the character's own set-position call (re-backlog M1), same frame
		kNative,           // that call every frame: Snake runs in place at the centre (M1 option 1)
	};

	enum class MusicMode
	{
		kKeep,     // Music = on
		kMuteBgm,  // Music = off (the old mute_bgm)
		kMuteAll,  // the old mute_all: Music, Ambience and Radio off
	};

	struct Config
	{
		// [Guest]
		bool debugLog{ false };

		// [Link]
		std::wstring mapping;  // empty = protocol default for this platform (Wine: /dev/shm file; Windows: named)

		// [Boot]
		bool        autoBoot{ true };
		int         mission{ 30020 };       // sandbox mission (MAFR_FREE: buddies allowed); 30050 MTBS_FREE, 30010 AFGH_FREE
		bool        hasPosition{ true };    // false = park wherever the mission puts Snake
		float       position[3]{ 820.6f, -11.1f, 1179.1f };  // Nova Braga Airport apron
		float       rotYDeg{ 90.0f };
		bool        disableSoldiers{ true };
		// MGSV's markers off (TppMarker.DisableAll) at setup and every ClearMarkersSec while Snake plays: a marked MGSV soldier near
		// the sandbox showed in Elden Ring's HUD layer ("32m") and pulled Snake's aim onto itself (live: a launcher's aim stuck
		// 30 deg off the Elden Ring target, its rockets flew there). 0 = never.
		int         clearMarkersSec{ 5 };
		// MGSV's "Auto-Aim Correction" (OPTIONS > Control Settings) while PhantomRing runs: off, because it snapped Snake's aim onto
		// the disabled MGSV soldiers standing around the sandbox (live: an FB MR launcher aimed 3-25 deg to their side turned to
		// them; with it off the aim stays put). In memory only, and only while MGSV's saves are blocked ([Saves] Block).
		bool        autoAim{ false };
		bool        invincible{ true };     // TppGameStatus S_DISABLE_PLAYER_DAMAGE: only Elden Ring hurts Snake
		std::string clockTime{ "12:00:00" };  // empty = keep MGSV's clock
		bool        freezeClock{ true };
		std::string weather{ "SUNNY" };     // TppDefine.WEATHER key; empty = keep
		bool        pressButtons{ true };   // pulse A at the title / loading tips
		int         timeoutSec{ 900 };

		// [Buddy]
		std::string buddy{ "walker" };  // none | walker | horse | dog | quiet
		float       buddyAhead{ 3.0f };   // called this far in front of Snake (m)...
		float       buddySide{ 3.5f };    // ...and this far to his right (negative: left), never between him and the camera
		bool        buddyFollow{ true };          // D-Walker: whistle it over (MGSV's own call command) when it falls behind
		// A buddy destroyed in MGSV's world (TppBuddyService dead) is revived and called again, at boot and when it is lost while
		// Snake plays: Elden Ring has no way to repair it, and the save is never written.
		bool        buddyRevive{ true };
		float       buddyFollowDistance{ 2.5f };  // ...this far from where it should walk (m)
		int         buddyFollowEveryMs{ 1000 };   // ...at most this often
		// Where a whistle sends it, relative to Snake as the camera looks at him (m): ahead (negative: toward the camera) and
		// to the right on screen (negative: left). Beside him, out of the camera line and out of his line of fire.
		// 0, 0 = Snake's own position, as MGSV's whistle does.
		float       buddyFollowAhead{ 1.0f };
		float       buddyFollowSide{ -2.5f };
		// The spot leads Snake along his velocity by this long (s, clamped to 8 m), so a running Snake has the walker
		// beside him, not catching up from behind; 0 = beside where he is.
		float       buddyFollowLeadSec{ 0.8f };
		// While D-Walker walks to its spot, move that spot with Snake (the walker AI re-reads it) instead of whistling again.
		bool        buddyFollowSteer{ true };
		bool        buddyFollowRespectCommands{ true };  // the player's own buddy command (a mode) pauses auto-follow until they whistle or pick Normal Mode
		// The spot's side offset while Snake runs (m, negative: left): MGSV's walker stops within ~3 m of a moving player.
		float       buddyFollowSideRunning{ -4.5f };
		// A walker this far behind a running Snake (m) is moved this close -- only between two places the camera can't see.
		// 0 = never.
		float       buddyFollowCatchUp{ 6.0f };
		// How auto-follow sends D-Walker (docs/GUEST.md 18.3, all three VERIFIED live): "record" = no message, its destination
		// record written directly (never a mode change, never a voice line; far walkers are still placed nearer), "command" =
		// MGSV's whistle command (puts the walker back into Normal Mode, which makes it announce the mode), "restore" = the
		// same whistle with the walker's mode kept.
		std::string buddyFollowMethod{ "record" };
		// The walker modes auto-follow acts in (normal stealth search intercept mode1 mode2, or all); any other mode the player
		// chose is left alone. Read from the walker's record once its controller is known (VERIFIED live: the call menu's
		// Search Mode -> "D-Walker mode normal -> search", left alone 74 m away; Normal Mode -> followed again).
		std::string buddyFollowModes{ "normal" };
		// D-Horse ([Buddy] Type = horse): MGSV's own "Whistle (call D-Horse)" (command 17) whenever it is farther than
		// HorseFollowDistance (m) from Snake on foot, at most every HorseWhistleEveryMs: it gallops over. 0 = never.
		float       buddyHorseFollowDistance{ 15.0f };
		int         buddyHorseWhistleEveryMs{ 6000 };
		// D-Horse treadmill (Horse.cpp, GUEST.md 8.1a; live pass B 2026-10-15: on): the treadmill moves D-Horse through its own location
		// object -- with Snake on foot by the same jump (no whistle), and *as* the jump while he rides it (no airport limit).
		bool        buddyHorseTreadmill{ true };
		// ... and while riding, Snake is also placed by the same jump in the same frame (in case the saddle doesn't carry him).
		bool        buddyHorseMoveRider{ false };

		// [Motion]
		HoldMode holdMode{ HoldMode::kNativeTreadmill };  // the Lua Warp turns MGSV's camera behind Snake (GUEST.md 19)
		float    radius{ 6.0f };
		float    teleportJump{ 3.0f };   // a one-frame jump bigger than this is a warp, not motion (m)
		int      warpCooldownMs{ 1000 };
		bool     treadmillOnVehicle{ true };
		bool     ridingCameraKeep{ true };   // turn MGSV's camera back after the vehicle's treadmill warp (it swung)
		bool     treadmillMovesBuddy{ true };
		float    velocitySmoothing{ 0.2f };  // EMA factor per frame
		float    warpLift{ 0.8f };           // added to the height Snake is warped/placed at (Warp lands that much low)
		bool     diveFlag{ true };           // publish MGSV's dive as kGuestDiving (Dive.h; the host's [Puppet] hop decides what it does)
		int      diveConfirmMs{ 600 };       // a sign of the dive must come this soon after X (Dive.h Tunables::confirmMs)

		// [Jump] Snake jumps (protocol v14 kGuestJumping; MGSV has no jump): the button's press lifts Snake in MGSV (the native
		// placement, Lift m up: MGSV plays its own fall and landing) and the host jumps the puppet through Elden Ring's world.
		bool          jump{ true };
		std::uint16_t jumpButton{ 0x0080 };    // XInput bits: R3 (the right stick's click). L3 is MGSV's DASH (sprint), taken
		std::uint16_t jumpPassWith{ 0x030F };  // held at the press, the button is MGSV's own: LB (call menu: R3 selects), RB (binoculars), the
		                                         // D-pad (its weapon / item wheels: R3 = Use, the Phantom Cigar's only way; live pass B 2026-10-15)
		bool          jumpPassAiming{ true };   // ... and LT (aiming: R3 changes the shoulder)
		float         jumpLift{ 1.0f };         // m (0.2-1.5: a small lift, MGSV's fall damage never starts)
		int           jumpMaxAirMs{ 2000 };     // the flag goes down by then at the latest (no landing seen)

		// [Pacing] (FramePace.h)
		int  jobSleepUs{ 500 };  // MGSV's own Sleep(1) (its job system's poll) as a delay of this many us; 0 = 1 ms as MGSV asks
		bool waitProbe{ false };  // detours under ntdll's waits for `!pace start` (diagnostics)

		// [Online]
		bool offline{ true };  // refuse MGSV's WinHTTP connections (Konami servers): MGSV plays offline

		// [Input]
		bool padDuringBoot{ false };
		bool hideDirectInputControllers{ true };
		bool rumble{ true };
		bool guardWindow{ true };

		// [Camera]
		float filmHeightMm{ 24.0f };  // focal (mm) -> vertical FOV: 2*atan(film/2/focal). UNVERIFIED
		float fovOverrideDeg{ 0.0f };
		float nearPlane{ 0.1f };

		// [Combat]
		float                      lifeScale{ 4.0f };  // MGSV life points per Elden Ring damage point (DESIGN 13b.5)
		bool                       nativeLifeWrite{ true };
		// MGSV's regeneration after an Elden Ring hit, the way it regenerates after its own (logic::RegenCap; live-measured):
		// hold RegenDelaySec, then a rate growing to RegenRate life/s over RegenRampSec. RegenGate = false: MGSV regenerates
		// from Elden Ring's hits at once (~760 life/s).
		bool                       regenGate{ true };
		float                      regenDelaySec{ 2.4f };
		float                      regenRampSec{ 6.25f };
		float                      regenRate{ 750.0f };
		bool                       hurtFeedback{ true };   // Elden Ring hits shake MGSV's camera (PR.Hurt, Player.RequestToSetCameraNoise)
		bool                       hurtBlood{ true };      // ...and put blood on Snake (Player.SetBloodToModel)
		bool                       deathCollapse{ true };  // Elden Ring death: Snake dives to the ground and lies there until the respawn (Death.cpp)
		bool                       fireFromAmmo{ true };   // fallback when the ShotSolver hook is refused
		bool                       nativeShots{ true };    // ShotSolver hook (re-backlog M3)
		bool                       melee{ true };          // CQC attack-serial poll -> kGEvMelee
		bool                       meleeShapeGeometry{ true };
		int                        muzzleBone{ 12 };    // right wrist in the 178-bone player skeleton
		std::vector<std::uint16_t> tranqWeapons;      // MGSV equip ids firing tranquilizer rounds
		std::vector<std::uint16_t> stunWeapons;
		bool                       blasts{ true };  // thrown/placed explosives -> kGEvExplosion (Blast.cpp)
		bool                       throws{ true };  // thrown explosives -> kGEvThrow, the aim arc -> GuestState (Throw.cpp)
		bool                       placed{ true };  // placed explosives -> kGEvPlaced/kGEvPlacedGone, kHEvDetonate sets them off (Placed.cpp)
		bool                       decoys{ true };          // (with placed) decoys -> kGEvPlaced a = kAmmoDecoy (roadmap 2.8, UNVERIFIED live)
		bool                       decoyPopInMgsv{ false };  // a decoy Elden Ring popped also gets MGSV's set-off bit (UNVERIFIED what it does)
		// Snake's own thrown explosives, the mines Elden Ring's enemies set off and his gun-fired rounds also blast in MGSV's sandbox
		// (Blast.cpp): there they can only hit D-Walker (Snake's MGSV damage is off) next to him; mines always lie within the
		// treadmill's disc, and a rocket fired with the walker in MGSV's line of fire destroyed it (live).
		bool                       mgsvBlasts{ false };
		// ---- C4's detonator (C4.cpp, docs/GUEST.md 18.8a) ----
		bool          c4Detonator{ true };    // MGSV's own detonator watched (its blast Elden Ring's); a press it refuses is the guest's
		bool          c4Backup{ true };       // a tap MGSV never answers is the guest's (live: MGSV's own detonator answered 0 of 5)
		std::uint32_t c4Buttons{ 0x20 };      // PlayerPad bits of the detonator (0x20 = ACTION: MGSV's own, Y by default)
		std::int64_t  c4GraceMs{ 250 };       // how long MGSV has to answer a tap before the backup sets one off
		bool          c4DetonateAll{ false }; // every live C4 per press (MGSV's own: the oldest only)

		// [Saves]
		bool         blockSaves{ true };
		std::wstring saveSinkDir{ L"PhantomRingSaveSink" };

		// [Audio]
		MusicMode music{ MusicMode::kMuteBgm };
		bool      ambience{ false };  // MGSV's environment sounds (wind, birds, insects); off also mutes the music
		bool      radio{ false };     // radio calls and the iDroid's voice announcements
		// D-Walker's own sounds as a gain on their Wwise objects (Sound.cpp). EXPERIMENTAL, no audible effect yet (GUEST.md
		// 18.2): 1 = off (no hooks); anything else installs the Wwise detours and the `!sound` diagnostics.
		float     walkerVolume{ 1.0f };

		// [Menu]
		bool safePauseMenu{ true };  // MGSV's pause menu keeps only Controls & Manual and Options (no checkpoint/ACC/title)
		bool blockMissions{ true };  // the iDroid can't start a mission or leave the area (the sandbox would be gone)
		bool buddyDismiss{ true };   // ...but its Buddy Support > DISMISS stays: the guest follows the player's choice (no buddy)
		// The iDroid hold (Start) the host sends: MGSV ignores Start while Snake aims, uses the binoculars or is in a dive/CQC/climb;
		// IDroidRetryMs > 0 keeps such a request and presses Start again (the rest of the pad neutral for that moment) until the
		// iDroid opens or this many ms passed. 0 = MGSV sees the host's Start only.
		int  idroidRetryMs{ 3000 };

		// [Loadout] TppEquip.EQP_* names (empty = the save's own). Weapons go into the sortie loadout before the sandbox
		// mission loads (MGSV loads their models); Support is given with Player.ChangeEquip at the setup.
		std::string loadoutPrimary{ "EQP_WP_East_ar_010" };  // PRIMARY_1 (hip): the SVG-76 assault rifle (lethal)
		std::string loadoutPrimary2{ "EQP_WP_West_ms_010" };  // PRIMARY_2 (back): the FB MR R-LAUNCHER (unguided rockets)
		std::string loadoutSecondary{ "EQP_WP_West_thg_010" };  // SECONDARY (handgun): the WU S.PISTOL, tranquilizer darts
		std::string loadoutSupport;    // SUPPORT, comma list (up to 8)
		std::string boxPoster{ "moe_v" };   // the poster put on Snake's first cardboard box (moe_v/moe_h anime, gravure_v/h, sol_*); empty = none
		std::string box{ "EQP_IT_CBox" };   // the box given into a free item slot when the save has none

		// [Capture]
		bool capture{ true };  // observe MGSV's D3D11 device and publish Snake's draws (phase 2)
		bool hud{ true };      // MGSV's own HUD as a transparent layer (Hud.cpp; needs capture)
		bool captureRevalidate{ true };  // re-key a texture/mesh whose copy was evicted (MGSV may reuse its address; Capture.cpp Stale())
		int  hudEvery{ 1 };    // publish the HUD layer every Nth frame
		// (v13) the grenade / rocket Snake just released published as kOwnerProjectile draws (ProjectileTag.h). UNVERIFIED live,
		// so off: a mis-tag takes a rigid draw away from Snake / the buddy (the host strips or re-owns kOwnerProjectile draws).
		bool projectileMeshes{ false };

		// [Proxies] ActorProxy v0 (ActorProxy.h, docs/GUEST.md §20). UNVERIFIED live; off by default.
		bool  proxies{ false };              // the sandbox's disabled soldiers stand in for the nearest hostile Elden Ring characters
		// [Cigar] (Cigar.cpp, docs/GUEST.md 18.16): the Phantom Cigar's time-lapse moves Elden Ring's clock (kGEvTimeSkip). UNVERIFIED live; off by default.
		bool  cigarTimeSkip{ true };         // [Cigar] TimeSkip (live pass B 2026-10-15: two smokes, 12:14 -> 18:02 -> 02:14 in Elden Ring): watch Snake smoke, send the minutes
		float cigarClockScale{ 20.0f };      // [Cigar] ClockScale: the frozen clock's time scale while he smokes, if MGSV's own time-lapse leaves it still (FreezeClock: 0; MGSV: 20)
		// [Fulton] (FultonAnchor.cpp, docs/GUEST.md 20.4): MGSV's own Fulton on a sleeping Elden Ring enemy (ActorTable kActorAsleep).
		bool  fultonAnchor{ true };          // [Fulton] Enabled: MGSV's Fulton on sleeping Elden Ring enemies
		// [Fulton] Method: proxy = an asleep proxy soldier is moved to the enemy (ActorProxy v1 for sleepers only, also with [Proxies]
		// off); anchor = a soldier near the sandbox is put to sleep and Snake placed beside him as by that enemy (20.4)
		std::string fultonMethod{ "proxy" };
		[[nodiscard]] bool FultonProxies() const { return fultonAnchor && fultonMethod == "proxy"; }
		// ActorProxy runs: for every hostile ([Proxies] Enabled) or only for sleeping ones ([Fulton] Method = proxy)
		[[nodiscard]] bool ProxiesRun() const { return proxies || FultonProxies(); }
		float fultonReach{ 3.0f };           // the enemy this near Snake (m, horizontal) starts it
		float fultonLeave{ 5.0f };           // Snake this far from the enemy (m) ends it (the soldier back, Snake back to the sandbox)
		float fultonCandidates{ 90.0f };     // anchor soldiers: standing within this of the sandbox centre (m) at its height
		float fultonRise{ 8.0f };            // the soldier this far above where he lay (m): MGSV's balloon took him -> kGEvFulton (a balloon
		                                     // that hits a roof drops him again after ~3-4 m: live, under the terminal's overhang)
		int   proxyMax{ 4 };                 // soldiers used as proxies; every other soldier and hostage is parked
		float proxyMaxDistance{ 60.0f };     // m from Snake (the host publishes characters within its [Actors] radius, 60)
		float proxySnakeClearance{ 3.5f };   // nearer to Snake's root than this: parked (Capture takes draws within 2.5 m as his)
		float proxyBuddyClearance{ 5.5f };   // nearer to the buddy than this: parked (Capture takes draws within 4 m as the buddy's)
		float proxyParkDepth{ 100.0f };      // parked soldiers wait this far below the sandbox centre
		int   proxyTableLagMs{ 33 };         // the ActorTable's age when it arrives (treadmill warps inside it are taken out)
		bool  proxyAutoAim{ true };          // MGSV's Auto-Aim Correction on while every pool soldier is placed (saves guarded)
		bool  proxyKeepMarkers{ true };      // no periodic TppMarker.DisableAll while proxies run (marking them is the point)
		float proxyCaptureRadius{ 1.5f };    // Capture drops G-buffer draws this near a proxy (and nearer to it than to Snake)
		// Inert full proxies (docs/GUEST.md 20.6; only with [Proxies] Enabled -- the Fulton-only proxies are unchanged). UNVERIFIED live.
		bool        proxyInert{ true };            // SetRestrictNotice + SetPuppet before and right after SetEnabled, refreshed every 2 s
		bool        proxyNoAssignRoute{ false };   // SetEnabled{noAssignRoute=true}: no route handed to a proxy when he is enabled
		bool        proxyPhaseLock{ true };        // MGSV's alert phase held at 0: sweep + command posts to sneak, give up after AlertGiveUpSec
		float       proxyAlertGiveUpSec{ 20.0f };  // an alert this long despite the lock: full proxies off for the session
		std::string proxyHeight{ "ground" };       // ground: proxies on the sandbox ground (x/z the enemy's); enemy: at the enemy's height
		float       proxyHeightTolerance{ 2.0f };  // read-back: a proxy this near the height it was put at (m) stands where it should
		bool        proxyAimHeightFix{ true };     // a round aimed at a flattened proxy goes to its enemy's real height (kGEvFire dir)

		// [Cqc] CQC holds and throws on Elden Ring enemies (ROADMAP 2.2 tier 1, Cqc.cpp / CqcLogic.h, protocol v13 kGEvCqc).
		// Only events: the host decides whether an Elden Ring enemy is there to take them (its own [Cqc] switch).
		bool          cqc{ true };                    // [Cqc] Enabled
		bool          cqcMgsvHold{ false };           // MGSV's CQC status / action callback starts a grab at once (UNVERIFIED: off)
		std::uint32_t cqcHoldMs{ 350 };
		std::uint32_t cqcChokeMs{ 2500 };             // 0 = only ChokeButtons choke
		std::uint32_t cqcMaxHoldMs{ 10000 };
		std::uint32_t cqcCooldownMs{ 400 };
		float         cqcThrowStick{ 0.6f };
		std::uint32_t cqcButtons{ 0x8010 };           // PlayerPad FIRE | CQC
		std::uint32_t cqcAimButtons{ 0x8 };           // PlayerPad HOLD
		std::uint32_t cqcInterrogateButtons{ 0x200 }; // PlayerPad CALL / INTERROGATE
		std::uint32_t cqcChokeButtons{ 0 };
		float         cqcRange{ 1.4f };               // m, kGEvCqc b (the host's own [Cqc] range decides)

		// [Lua]
		std::wstring luaOverride{ L"phantomring.lua" };
		bool         dumpDefaultLua{ true };

		void Load(const std::wstring& a_path);
	};

	const Config& Cfg();
	Config&       MutableCfg();
}
