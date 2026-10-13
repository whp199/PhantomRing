#pragma once

// PhantomRingHost.ini (next to the DLL). Every key is documented in er/PhantomRingHost.ini; missing
// keys take the defaults below.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	// [Render] (phase 2: MGSV's meshes drawn in Elden Ring; er/Renderer.h)
	struct RenderConfig
	{
		bool        enabled{ true };
		bool        drawPlayer{ true };
		bool        drawBuddy{ true };
		bool        drawOther{ true };
		int         latencyFrames{ 0 };      // snapshot age used at Present (0 = newest)
		std::string cull{ "none" };          // none | back | front (MGSV's head and launcher need none: live 2026-10-12)
		bool        frontCcw{ false };       // MGSV's front-face winding (D3D default: clockwise)
		bool        flipX{ false };          // mirror the projection horizontally (if row 0 of pers_cam_1 is screen-left)
		float       lightDir[3]{ 0.35f, 0.8f, 0.45f };
		float       lightColor[3]{ 1.0f, 0.96f, 0.9f };
		float       lightIntensity{ 2.2f };
		float       ambientUp[3]{ 0.42f, 0.45f, 0.5f };
		float       ambientDown[3]{ 0.2f, 0.18f, 0.15f };
		float       exposure{ 1.0f };
		bool        dayNight{ true };        // follow Elden Ring's clock: sun arc by day, dim blue moonlight by night
		bool        albedoSrgb{ true };      // UNORM albedo textures hold sRGB values
		std::string normalXy{ "auto" };      // auto | rg | ag (normal map channels)
		float       hdrPaperWhite{ 200.0f };  // nits of 1.0 on an HDR swap chain
		int         debugView{ 0 };          // 0 lit, 1 albedo, 2 normals, 3 uv, 4 facing, 5 bones, 6 SRM
		float       xray{ 0.0f };            // > 0: draw see-through with this opacity (alignment checks against the debug skeleton)
		bool        erDepth{ true };         // hide Snake behind Elden Ring's scene depth (er/RenderDepth.h)
		float       erDepthBias{ 0.004f };   // relative: drawn when viewZ <= erZ * (1 + bias)
		float       erDepthNear{ 0.0f };     // near plane Elden Ring's depth was made with; 0 = pers_cam_1's
		bool        probe{ true };           // watch Elden Ring's render targets for `render probe` (stage B research)
		// Where Snake is drawn: gbuffer = into ER's G-buffer so its own lighting lights him (stage C; falls back
		// to post for a frame whose structure doesn't match, logged); post = into ER's tone-mapped scene
		// before its HUD (stage B); present = over the finished frame (stage A).
		std::string stage{ "gbuffer" };
		// Stage C (er/Gbuffer.h). Values Elden Ring's characters carry that MGSV's assets don't, measured on
		// the Tarnished with `render gbuf peek`.
		std::string gbufferDepth{ "finite" };  // finite (pers_cam_1 near/far) | infinite reversed Z
		int         gbufferStencil{ 0x50 };     // stencil class bits Elden Ring's lighting tests
		int         gbufferLightSet{ 0 };       // RT2.a: index into the light-set table; 0 = Elden Ring's world set (always lit);
		                                        // -1 = auto (learned from the Tarnished, else 3: black with no ER character in view)
		float       gbufferAo{ 1.0f };          // RT1.w (characters: vertex colour red)
		float       gbufferTranslucency{ 0.0f };  // RT1.z (foliage wrap lighting; 0 for characters)
		float       albedoScale{ 1.0f };
		float       glossScale{ 1.0f };          // gloss = (1 - roughness)^2 * scale + bias
		float       glossBias{ 0.0f };
		float       metalScale{ 1.0f };          // SRM blue as metalness
		std::string alphaTest{ "auto" };         // auto: diffuse textures with alpha (BC2/BC3/BC7) are alpha-tested | flag: only kDrawAlphaTest
		float       alphaRef{ 0.5f };
		std::vector<std::uint32_t> camoMaterials{ 0x5216a6bb };  // guest material ids painted with t3 (camo) where t4 (mask) is set
		std::vector<std::uint32_t> posterMaterials{ 0xc94150c9 };  // guest material ids with MGSV's box poster: t4 on the second UV set
		bool        velocity{ true };            // write Snake's motion vectors after ER's velocity pass
		bool        smoothDraws{ true };         // blend Snake's and the buddy's matrices between MGSV's draw lists (er/Pacing.h; needs [Camera] smooth)
		bool        viewConstants{ true };       // project with Elden Ring's own view constants (er/RenderView.h) once they match its camera
		bool        shadows{ true };             // Snake casts shadows into Elden Ring's shadow atlas (needs view_constants)
		float       velocityScale[2]{ 0.5f, -0.5f };  // NDC delta -> ER velocity units
		float       velocityBias[2]{ 0.0f, 0.0f };
		// MGSV-style near-camera dither fade: gone at [0] metres from the camera, solid from [1]; Snake
		// per pixel, everything else (D-Walker...) also as a whole by its nearest part.
		float       nearFade[2]{ 0.25f, 0.6f };
		float       nearFadeOther[2]{ 0.5f, 1.5f };
		// MGSV fades what stands between its camera and Snake: a non-player object with a part within
		// occluder_radius of the camera-to-Snake line, in front of him, is drawn at occluder_fade (not while riding).
		float       occluderFade{ 0.15f };
		float       occluderRadius{ 1.0f };
		// Developer: draw Snake this far (ER world, m) from the puppet, e.g. beside the Tarnished for a
		// lighting comparison (`render set show_tarnished on`). Console only, never in the ini.
		float       debugOffset[3]{ 0.0f, 0.0f, 0.0f };
		int         uploadMbPerFrame{ 64 };
		// What Snake looks like from the moment Elden Ring's player dies (Snake's death or ER's own) until
		// the respawn: fade (dissolve away over death_fade_ms), keep (MGSV keeps drawing him: for when the
		// guest plays a fall/faint animation), cut (gone at once).
		std::string deathVisual{ "keep" };  // the guest plays Snake's collapse ([Combat] DeathCollapse)
		int         deathFadeMs{ 900 };
		// MGSV's night-vision goggles recreated on Elden Ring's frame (GuestState kGuestNvg, er/RenderNvg.h): its tone-mapped
		// scene (Snake included) before its HUD becomes a bright green picture.
		bool        nvg{ true };
		float       nvgGain{ 5.0f };           // luminance^curve x this, then soft highlight compression: 1 - exp(-gain * L^curve)
		float       nvgCurve{ 0.6f };          // < 1 lifts the dark more than the light (1 = linear: a dark tunnel stayed near black)
		float       nvgFloor{ 0.025f };        // black level lift (share of full brightness)
		float       nvgTint[3]{ 0.30f, 1.0f, 0.40f };  // linear RGB of full brightness
		float       nvgGrain{ 0.07f };         // per-pixel noise, changing every frame (share of full brightness)
		float       nvgVignette{ 0.35f };      // darkening toward the corners (0 = off)
		float       nvgScanlines{ 0.0f };      // darkening of every other line pair (0 = off)
		int         nvgFadeMs{ 250 };          // in/out as the goggles go on and off

		bool operator==(const RenderConfig&) const = default;
	};

	// [Overlay] (phase 5: MGSV's HUD layer, fallback reticle, corner status; er/Overlay.h, er/Hud.h)
	struct OverlayConfig
	{
		bool        enabled{ true };
		bool        hud{ true };              // composite the guest's HUD layer (HudCtl triple buffer)
		int         hudFadeMs{ 350 };
		std::string reticle{ "auto" };        // auto (while no MGSV HUD layer arrives) | on | off
		float       reticleScale{ 1.0f };
		bool        status{ true };           // MGSV boot progress / absent / failed, in a corner
		bool        messages{ true };         // short event messages (Snake in control, MGSV stopped)
		float       messageSeconds{ 4.0f };
		float       absentSeconds{ 12.0f };   // how long "MGSV is not running" stays up
		float       textScale{ 1.0f };
		std::string corner{ "top_right" };    // top_right | top_left | bottom_right | bottom_left
		// MGSV's HUD layer next to Elden Ring's (er/HudLayout.h): whole-layer scale/offset, and two regions of the
		// layer (x0 y0 x1 y1, fractions of it) moved on their own (fractions of the screen).
		float       hudScale{ 1.0f };
		float       hudOffset[2]{ 0.0f, 0.0f };
		float       hudWeaponRegion[4]{ 0.78f, 0.70f, 1.0f, 1.0f };   // weapon/item box (bottom right)
		float       hudWeaponBarRegion[4]{ 0.62f, 0.84f, 1.0f, 1.0f }; // the bar row under it (reaches x 0.66), moved with it
		float       hudWeaponOffset[2]{ 0.0f, -0.05f };                // up: clear of Elden Ring's rune counter and its "+N"
		float       hudNotifyRegion[4]{ 0.0f, 0.30f, 0.40f, 0.72f };  // notifications (left; the centre stays: reticle)
		float       hudNotifyOffset[2]{ 0.0f, 0.0f };

		bool operator==(const OverlayConfig&) const = default;
	};

	struct Config
	{
		// [Link]
		std::wstring mapping;

		// [Debug]
		bool debug{ false };
		bool verifySha256{ false };

		// [Features]
		bool input{ true };
		bool puppet{ true };
		bool camera{ true };
		bool console{ true };
		bool actors{ true };
		bool debugDraw{ true };
		bool buddy{ true };

		// [Input]
		int           userIndex{ -1 };          // XInput pad published to MGSV; -1 = first one that answers
		std::uint16_t allowButtons{ 0x0030 };   // passed to ER while puppeting (START | BACK/View)
		bool          menuLearn{ true };
		std::vector<int> menuUiStates;          // CSMenuMan UIState indices that mean "an ER menu is open"
		int           menuLatchMs{ 1500 };      // after Start/View: assume a menu opened for this long
		std::uint16_t promptButton{ 0x8000 };   // Y (ER's "event action" on an Xbox pad, VERIFIED live): goes to ER (not MGSV) while ER shows an interaction prompt; 0 = off
		int           interactionHudState{ 7 };  // CSMenuMan UI state hidden while ER runs an interaction (grace rest, VERIFIED live); -1 = none
		int           interactionMinMs{ 3500 };  // after a prompt press ER keeps the controller at least this long (a pickup; a grace hides its HUD after ~3 s)
		int           interactionMaxMs{ 0 };     // ...and at most this long (0 = until the HUD is back)
		std::vector<int> releasePrompts{ 10000 };  // ActionButtonParam ids where Elden Ring walks the Tarnished itself (the mist): the puppet steps aside
		int              releaseMs{ 4500 };        // ...for this long after the press, then Snake takes over where ER left him
		int           viewHoldMs{ 400 };         // View held this long while Snake plays = MGSV's iDroid (a tap stays ER's map); 0 = off
		int           guestIdroidStartMs{ 1200 };  // ...sent to MGSV as Start held this long (MGSV: Back = pause menu, Start held ~1 s = iDroid, VERIFIED live)
		bool          guestMenuOwnsPad{ true };  // kGuestInMenu: MGSV gets the whole pad, Elden Ring nothing
		int           padGlyphDevice{ 0 };       // Elden Ring's glyph device while Snake plays: 0 pad, 1 keyboard; -1 = leave it to ER
		int           startWaitMs{ 150 };        // Start (and View in Tarnished mode) reaches Elden Ring this long after it went down: room for the chord
		int           chordMs{ 1000 };           // View + Start held together this long toggles Tarnished mode (er/Chord.h); 0 = off
		std::vector<int> restPrompts{ 6101 };    // ActionButtonParam ids of "Rest at site of grace": its HUD hiding after it = a rest (kHEvRested 0)
		std::vector<int> menuKeys{ 0x1B, 0x47 }; // keyboard keys that open Elden Ring menus (Esc, G = map): count like a Start press (VK codes)
		std::vector<int> ladderPrompts{ 5000, 5010 };  // ActionButtonParam ids "Climb" / "Descend" (ladders, er/Ladder.h): Elden Ring climbs
		int              ladderPressDelayMs{ 100 };    // ...the puppet steps aside on the press, which reaches Elden Ring this much later
		int              ladderWaitMs{ 1500 };         // ...until the climb starts (at most this long after the press)
		int              ladderSettleMs{ 300 };        // ...and back this long after the Tarnished got off the ladder

		// [Puppet]
		std::string hideMode{ "render_flag" };  // render_flag | fade | none
		std::string motion{ "kinematic" };      // kinematic (default: plays well) | root_motion (ER's own proxy: invisible walls in real play, 2026-10-12)
		float catchUp{ 0.6f };      // root_motion: m Elden Ring's proxy may fall behind Snake before the rest is dropped (it moves 60-95% of what it
		                            // is handed, less uphill: the shortfall is handed again, up to this much); 0 = off
		bool  unstick{ true };      // kinematic: a puppet that is boxed in (inside a cart, a rock) or stuck for unstick_seconds goes back to
		float unstickSeconds{ 0.5f };  // where it last stood free
		float fallWatchSeconds{ 4.0f };  // a fall with no landing after this long (s) ... (NoDead: a chasm's kill volume can't kill the puppet)
		float fallWatchDrop{ 60.0f };    // ... or this deep (m) is Snake's death (YOU DIED); 0 = that check off
		bool  hitHook{ true };     // catch ER hits in ReceiveDamage and cancel them (re-backlog E2)
		bool  noDead{ true };
		bool  noDamage{ false };   // NoDamage hides hits from us (we detect hits by HP loss), so off by default
		bool  mirrorLife{ true };  // ER HP bar shows Snake's life fraction
		bool  forwardHeals{ true };  // ER heals on the puppet raise Snake's life by the same share (kHEvHeal)
		bool  killOnGuestDeath{ true };
		float maxSpeed{ 12.0f };          // m/s on foot; larger single-frame odometer jumps are clamped
		float maxSpeedVehicle{ 32.0f };   // m/s while riding (D-Walker dash/hover)
		float maxSpeedHorse{ 32.0f };     // m/s while riding D-Horse (its gallop)
		float teleportDrop{ 4.0f };       // an odometer jump larger than this (m) in one frame is a guest warp: ignored
		float resyncDistance{ 1.5f };     // ER moved the puppet further than this from where we put it: re-anchor
		float radius{ 0.35f };
		float stepUp{ 0.5f };
		float stepUpVehicle{ 0.8f };
		float stepUpHorse{ 0.8f };
		float radiusVehicle{ 0.35f };     // the kinematic body's radius while riding D-Walker (or another vehicle)
		float radiusHorse{ 0.45f };       // ... and D-Horse (longer than Snake: its head and tail)
		float snapDown{ 0.6f };
		float chestHeight{ 1.3f };
		float gravity{ 9.81f };
		std::uint32_t rayFilter{ 0x2000058 };  // cs_phys_world_cast_ray filter (the only in-game caller uses this)

		// [Axes] (UNVERIFIED conventions; confirmed live and pinned here)
		float mirrorX{ -1 };         // Fox and Havok differ in handedness (verified live 2026-10-12)
		float mgsvYawSign{ 1 };
		int   erForward{ 0 };       // 0 = auto (learned from the Tarnished's own running), else +1/-1
		float mgsvCamForward{ 1 };
		float mgsvRootOffsetY{ 0 };  // height of GuestState.rootPos above Snake's feet (m)

		// [Camera]
		int   cameraTaskGroup{ 107 };   // DrawParamUpdate
		int   camForwardRow{ 0 };       // 0 = auto (vote from ER's own camera), else +1/-1 for the +0x30 row
		float fovScale{ 1.0f };
		float fovOffset{ 0.0f };
		bool  writeNear{ true };
		bool  cameraCollision{ true };
		bool  syncChrCam{ true };       // also write the camera into ER's own player camera (WorldChrMan.chr_cam): enemy update LOD, 3D sound
		bool  syncCompassCam{ true };   // and into chr_cam again right before ER computes its HUD compass from it (CameraDriver::ApplyCompass)
		float eyeHeight{ 1.6f };
		float eyeHeightVehicle{ 2.2f };
		float eyeHeightHorse{ 2.4f };
		float collisionMargin{ 0.25f };
		float collisionRadius{ 0.15f };   // extra rays this far around the camera (0 = the centre ray only)
		float collisionEaseOut{ 0.2f };   // s: back out after a pull-in (in is immediate)
		float verticalSmoothing{ 0.06f }; // s: the camera follows the puppet's feet height eased (ground snap on steps); 0 = rigid
		// Frame pacing (er/Pacing.h): MGSV's frames blended into Elden Ring's (camera, puppet).
		bool  smooth{ true };
		float smoothDelay{ -1.0f };       // MGSV steps behind its newest frame; -1 = auto (follows how late its frames come)
		float smoothExtrapolate{ 1.5f };  // steps past the newest frame carried on when the next is late (an MGSV hitch)
		int   smoothMotion{ 7 };          // MGSV frames the odometer's straight-line fit spans (animation-key steps); < 3 = plain blend

		// [Actors]
		float actorRadius{ 60.0f };
		int   actorMax{ 64 };

		// [Draw]
		bool  drawSkeleton{ false };  // developer views: off for players, `draw` console command / ini to turn on
		bool  drawBuddy{ false };
		bool  drawTracers{ false };
		float tracerSeconds{ 0.5f };

		// [Combat] (docs/COMBAT.md)
		float         traceRange{ 150.0f };
		bool          damage{ true };             // Snake's shots/CQC/explosions hurt Elden Ring characters
		std::string   route{ "auto" };            // auto | bullet | hp
		std::string   targets{ "hostile" };       // hostile (ER's own team relation) | all
		std::uint32_t bulletId{ 10170000 };       // BulletParam row spawned at the target (Throwing Dagger's)
		bool          bulletHideSfx{ true };
		float         bulletOffset{ 0.6f };       // spawn this far before the hit point (m)
		float         bulletDamageMult{ 1.9f };   // atkPhys = wanted damage x this (ER defence eats part of it)
		int           verifyMs{ 300 };
		int           verifyFailures{ 3 };
		// Damage in MGSV soldiers' lives x a reference HP per target (combat::Scaling, docs/COMBAT.md §2).
		float         unitsPerLife{ 40.0f };      // class-table units (no MGSV share) in one soldier's life
		float         bigFrom{ 1.5f };            // max HP / the Tarnished's max HP up to which an enemy is ordinary (reference = its max HP)
		float         bossFrom{ 5.0f };           // ... from which it is boss-like (worth boss_lives soldiers)
		float         bossLives{ 45.0f };       // MGSV's assault-rifle share (0.2115 a round): a magazine ~15% of a boss (live, Margit)
		float         shareMult{ 1.0f };          // x MGSV's own damage share (protocol v8)
		float         headFraction{ 0.82f };
		float         headshotMult{ 4.5f };       // ordinary enemies (MGSV: a headshot kills a soldier)
		float         headshotMultBoss{ 1.5f };
		float         headshotMinLives{ 1.05f };
		float         buddyMult{ 1.0f };
		int           maxDamage{ 9999 };
		int           tranqSleep{ 80 };
		int           tranqSleepHead{ 200 };
		int           chokeSleep{ 200 };
		int           gasSleep{ 40 };             // per MGSV sleep-gas report (~every 0.5 s while its cloud lasts)
		float         tranqShare{ 0.6f };         // the same as shares of the target's own sleep resistance, when it can be read:
		float         tranqShareHead{ 1.1f };     // 2 darts in the body or 1 in the head, a choke, the gas within ~3 s
		float         chokeShare{ 1.1f };
		float         gasShare{ 0.2f };
		int           sleepCorrection{ 0 };       // AtkParam correction (%) of a sleep-only hit (darts, gas): 0 = no damage, still lands
		float         stunPoiseMult{ 1.0f };
		int           sleepSpEffectBase{ 25000 };
		int           sleepSpEffectStep{ 10 };
		int           sleepSpEffectCount{ 20 };
		float         meleePunch{ 5.0f };
		float         meleeKick{ 7.0f };
		float         meleeThrow{ 4.0f };
		float         meleeThrowPoise{ 80.0f };
		float         meleeStrikePoise{ 25.0f };
		float         meleeRange{ 1.6f };
		float         meleeArcDeg{ 60.0f };       // half angle in front of Snake
		float         explosionRadius{ 4.0f };
		float         maxHitsPerSecond{ 30.0f };
		float         capsuleRadius{ 0.5f };      // when a character's physics module values look wrong
		float         capsuleHeight{ 1.8f };
		std::string   fallDamage{ "auto" };       // auto (host computes it in kinematic motion) | host | off
		float         fallHurtSpeed{ 12.5f };     // m/s at landing (~8 m drop)
		float         fallLethalSpeed{ 19.8f };   // ~20 m drop
		float         guestLifeScale{ 4.0f };     // must equal the guest's [Combat] life_scale
		bool          topUp{ true };              // bullet route: complete the wanted damage with ChangeHp (combat::TopUp)
		bool          hurtShare{ true };          // ER hits cost Snake the share of his life they'd cost the Tarnished
		float         hurtMult{ 1.0f };           // x this
		// Explosion visuals (er/Explosion.h): Elden Ring pot bursts at MGSV's blasts. Order: lethal, stun, sleep, smoke.
		std::string                  explosionFx{ "auto" };  // auto | bullet | sfx | off
		// Order: lethal, stun, sleep, smoke, flame (D-Walker's flamethrower: the Dragonfire incantation's flames), breaker and shrapnel
		// (no look: the flying Fire Pot's bullet carrying an object-only attack, light / heavy: they break Elden Ring's destructible
		// assets).
		std::array<std::uint32_t, 7> explosionTemplate{ 10030001, 10035001, 10064001, 10033001, 10700000, 10030000, 10030000 };  // BulletParam rows
		std::array<std::int32_t, 7>  explosionSfx{ -1, -1, -1, 300192, -1, -1, -1 };  // flight effect instead of the row's own (-1 = its own)
		std::array<std::uint32_t, 7> explosionCarrier{ 10048000, 10048001, 10302000, 10302001, 10302002, 10302003, 10302004 };  // cut-content rows
		int                          explosionMaxBursts{ 7 };       // per blast (centre + a ring)
		float                        explosionPerSecond{ 40.0f };   // bursts per second, all kinds
		float                        explosionCloudRepeat{ 2.5f };  // s between two bursts of one lingering cloud
		bool                         explosionGround{ true };       // MGSV-mapped blasts and clouds sit on Elden Ring's ground
		int                          explosionMaxLive{ 48 };        // our effect bullets alive at once (Elden Ring's bullet pool is shared)
		float                        explosionCloudLife{ 6.0f };    // s one sleep-mist / smoke burst lasts (the cloud is refreshed when it ends)
		// Destructible assets (crates, pots, barrels, carts): an object-only attack (AtkParam_Pc breaker_atk: no damage, no
		// characters, atk_obj) on the breaker look and the lethal blasts.
		bool          breakObjects{ true };
		std::uint32_t breakerAtk{ 10048000 };      // AtkParam_Pc row patched into the object-only attack (the cut Roped Freezing Pot's)
		int           breakerObjAttack{ 10 };       // its atk_obj (a Fire Pot burst's: pots, crates, bushes -- defence 0 -- break)
		std::uint32_t explosionAtk{ 10048001 };    // ...and the blasts' (the cut pot's burst row): atk_obj 100, meant for defence 90/95
		int           explosionObjAttack{ 100 };    // (fences, big crates); live, a defence-90 fence stayed up even at 999 (docs/HOST.md)
		float         breakerRadius{ 0.15f };       // m: the breaker's hit radius (the flying Fire Pot's: 0.08)
		float         breakerSpeed{ 25.0f };        // m/s it flies through the point (the flying Fire Pot's), from breaker_lead m before it
		float         breakerLead{ 0.6f };
		int           shrapnelCount{ 8 };           // heavy breakers flying out of a lethal blast (+ one down), over its radius
		bool          breakerHitsCharacters{ true };   // AtkParam oppose-target bit kept (as on Elden Ring's own flying pot: a 0-damage touch)
		bool          explosionBreaks{ true };      // lethal blasts send shrapnel (heavy breakers) out to their radius
		float         breakerPerSecond{ 20.0f };
		// Explosive rounds fly (er/Rounds.h): m/s from the muzzle to the impact (0 = at once). MGSV's missile launcher: its
		// blast came 0.9-1.1 s after the shot ~16-22 m away (session 4), 3.3-3.5 s at 150 m (the guest's measurement: ~0.8 s +
		// distance / 60 m/s): a rocket leaves at round_speed_rocket and its motor takes it to round_speed_rocket_boost after
		// round_rocket_boost_after s; grenade rounds ~100 m/s.
		bool          roundFlight{ true };
		float         roundSpeedRocket{ 16.0f };
		float         roundSpeedRocketBoost{ 60.0f };
		float         roundRocketBoostAfter{ 1.0f };
		float         roundSpeedGrenade{ 80.0f };
		bool          flameFx{ true };              // D-Walker's flamethrower drawn with Elden Ring flames
		float         flamePerSecond{ 6.0f };
		float         flameSpeed{ 14.0f };          // m/s the flames fly (0 = the template's own)

		// [Stealth] Snake in MGSV's cardboard box (protocol v8 kGuestInBox, er/Stealth.h)
		bool          boxHide{ true };
		std::int32_t  stealthSpEffect{ 20004999 };  // SpEffectParam: enemies' sight and hearing of the Tarnished x 0 (no visual, no end)
		float         boxStillSpeed{ 0.4f };       // m/s: Snake keeps still in the box
		float         boxSlowSpeed{ 1.8f };        // m/s: ...or moves this slowly for box_slow_grace seconds
		float         boxSlowGrace{ 1.5f };
		float         boxRevealDistance{ 2.0f };   // a hostile this close sees through the box
		bool          boxPoster{ true };           // kGuestBoxPoster: enemies that see it stop and stare
		float         boxPosterRange{ 15.0f };
		float         boxPosterStare{ 4.0f };      // s
		float         boxPosterCooldown{ 12.0f };  // s before the same enemy stares again

		// [Throw] Snake's thrown explosives flown through Elden Ring's collision (er/Throw.h)
		bool        throwSim{ true };            // fly our own copy of each kGEvThrow and blast where it is
		float       throwGravity{ 9.8f };        // m/s^2 (MGSV's, protocol v6)
		float       throwRestitution{ 0.3f };
		float       throwFriction{ 0.65f };
		float       throwRollDecel{ 3.0f };      // m/s^2
		float       throwImpactWait{ 1.5f };     // s an impact grenade waits for our copy to land after MGSV's went off
		float       throwRadiusLethal{ 7.0f };   // m, until MGSV reported the equip's own radius
		float       throwRadiusSleep{ 9.0f };
		float       throwRadiusStun{ 6.0f };
		float       throwRadiusSmoke{ 8.0f };
		float       throwBallRadius{ 0.06f };    // the drawn grenade (m)
		std::string throwArc{ "auto" };          // auto (while MGSV says Snake aims a throwable) | off
		float       throwArcSpacing{ 0.3f };     // m between the arc's dots
		float       throwArcDot{ 0.025f };       // m, a dot's radius
		float       throwArcRing{ 0.55f };       // m, the landing ring
		float       throwArcColor[4]{ 0.62f, 0.9f, 1.0f, 0.95f };  // linear RGB + alpha

		// [Mines] Snake's placed explosives (er/Mines.h)
		bool  mines{ true };                     // track kGEvPlaced, draw them, set them off for Elden Ring's enemies
		float mineArmSeconds{ 1.5f };            // after placing, before an enemy can set one off
		float mineBodyRadius{ 0.07f };
		float mineLightRadius{ 0.035f };

		// [Progression] (docs/COMBAT.md "Progression"): Elden Ring's progression scales Snake's damage. Off by default.
		std::string progression{ "off" };     // off | level | weapon | both
		float       levelReference{ 20.0f };  // the Tarnished's rune level at which Snake deals MGSV's own damage (x1)
		float       levelExponent{ 0.5f };    // level: x (level / level_reference)^level_exponent
		float       weaponBonus{ 1.5f };      // weapon: x (1 + weapon_bonus x upgrade / max upgrade): +25 (+10 somber) = x2.5
		float       progressionMin{ 0.5f };   // the factor is kept within these
		float       progressionMax{ 3.0f };

		RenderConfig  render;
		OverlayConfig overlay;

		bool Load(const std::wstring& a_path);
	};

	Config& Cfg();
}
