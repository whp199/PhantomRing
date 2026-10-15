#include "Puppet.h"

#include "Addresses.h"
#include "Buddy.h"
#include "Calibration.h"
#include "Combat.h"
#include "CombatLogic.h"
#include "Explosion.h"
#include "Fulton.h"
#include "Config.h"
#include "DiveHop.h"
#include "HitHook.h"
#include "Hud.h"
#include "Link.h"
#include "Log.h"
#include "Mem.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Revival.h"
#include "RootMotion.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <windows.h>

namespace phantom::er::Puppet
{
	namespace
	{
		struct Saved
		{
			bool          renderBit{ true };
			float         transparencyMod{ 0 };
			float         baseTransparency{ 1 };
			std::uint8_t  dataDebug{ 0 };
		};

		bool           g_active{ false };
		std::uint8_t   g_holds{ 0 };  // Hold bits: the puppet runs only while none is set
		std::uint8_t*  g_chr{ nullptr };  // the ChrIns we puppet (stop if the game swaps it)
		Anchor         g_anchor;
		KinematicState g_kin;
		Vec3           g_lastSet;
		double         g_odoLast[3]{};
		std::uint32_t  g_odoEpoch{ 0 };
		bool           g_riding{ false };
		bool           g_horse{ false };          // ...and what Snake rides is D-Horse ([Puppet] *_horse, [Camera] eye_height_horse)
		bool           g_wasRiding{ false };
		Saved          g_saved;
		std::int32_t   g_hpLast{ 0 };             // the HP we set last frame (the mirror of Snake's life)
		std::int32_t   g_maxHpLast{ 0 };
		double         g_healCarry{ 0 };          // fraction of a kHEvHeal unit not sent yet (regeneration ticks)
		std::uint64_t  g_heals{ 0 };
		std::int64_t   g_healedHp{ 0 };
		float          g_hpFrac{ 1 };
		float          g_landingSpeed{ 0 };
		bool           g_wasDead{ false };
		bool           g_killedByGuest{ false };
		bool           g_deathPending{ false };   // Snake died while the puppet was off: kill once it is back
		ULONGLONG      g_killAt{ 0 };             // when we last asked ER to kill the puppet (verification)
		int            g_killTries{ 0 };
		std::uint64_t  g_fallSentFrame{ 0 };      // host-computed fall damage went out on this frame
		std::uint64_t  g_falls{ 0 };
		std::uint64_t  g_frames{ 0 };
		std::uint32_t  g_dropped{ 0 };
		bool           g_rootMotion{ false };    // motion = root_motion and the hook is installed
		double         g_wantedM{ 0 };            // horizontal metres Snake's motion asked for / the puppet really moved (console: puppet)
		double         g_movedM{ 0 };
		// Walls (both motions; the root-motion vs kinematic comparison, docs/HOST.md "Motion"): frames Snake asked to move and the
		// puppet was blocked (kinematic: its body met geometry; root motion: the proxy moved < 30% of what it was handed), and
		// how many times that began.
		std::uint64_t  g_wallFrames{ 0 };
		std::uint64_t  g_walls{ 0 };
		bool           g_wasBlocked{ false };
		std::uint64_t  g_bumps{ 0 };              // kinematic: object-only breakers sent where the body met something
		// Root motion: Elden Ring's proxy moves 60-95% of what it is handed (less uphill; VERIFIED live, 2026-10-12): the shortfall
		// is handed again ([Puppet] catch_up), and dropped when the proxy is blocked (a wall) so nothing pops round a corner.
		Vec3           g_rmDebt;                  // Snake's motion asked for minus what the proxy did (horizontal)
		Vec3           g_rmAsked;                 // last frame's step of Snake's own
		Vec3           g_rmHanded;                // ...and what was handed to the proxy with the catch-up
		ULONGLONG      g_rmStillSince{ 0 };
		Unstick        g_unstick;                 // kinematic: boxed in -> back to where it stood free
		std::uint64_t  g_ceilings{ 0 };           // kinematic: frames a move was refused under a ceiling lower than Snake
		FallWatch      g_fallWatch;               // a fall that never lands (a chasm's kill volume under NoDead) -> Snake dies
		std::uint64_t  g_endlessFalls{ 0 };
		// Dive = hop (roadmap 2.9, [Puppet] hop): MGSV's dive lifts the kinematic puppet over a gap or onto a ledge.
		DiveTrigger    g_dive;
		std::uint64_t  g_hops{ 0 }, g_hopsRefused{ 0 }, g_hopsNoMove{ 0 };
		std::uint64_t  g_groundLost{ 0 };         // kinematic: walked off ground (each logged with what the two filters see below)
		ULONGLONG      g_lastBump{ 0 };           // kinematic: the last object-only attack against what blocked the body
		ULONGLONG      g_rootMotionSince{ 0 };
		// Death visuals: from the kill (or Elden Ring's own death of the puppet) until the respawn Snake
		// fades out (er/RenderScene) and the Tarnished stays hidden -- he must not die on screen in Snake's place.
		ULONGLONG      g_deathAt{ 0 };            // 0 = nobody is dying
		std::uint8_t*  g_deathChr{ nullptr };
		bool           g_deathLoad{ false };      // a load (or another player object) came since the death
		ULONGLONG      g_aliveSince{ 0 };         // the player has been alive again since (0 = dead / not yet)
		bool           g_killConfirmed{ false };  // ER's dead bit was seen after our Kill
		bool           g_hiddenForDeath{ false };  // the puppet stopped with the Tarnished still hidden (g_saved holds the originals)
		Vec3           g_deathPos{};               // where the dying player was (er/Revival.h: a revival moves it)
		bool           g_deathPosValid{ false };
		// Snake's jumps (protocol v14 kGuestJumping, the guest's [Jump] button): the flag last frame (a jump starts on its rising
		// edge), the puppet's running velocity on the ground (the takeoff's), and counts for the log and `puppet`.
		JumpEdge       g_jumpEdge;
		Vec3           g_runVel;
		std::uint64_t  g_jumps{ 0 };
		std::uint64_t  g_jumpsRefused{ 0 };
		bool           g_jumpRootWarned{ false };

		Axes CurrentAxes()
		{
			Axes a;
			a.mirrorX = Cfg().mirrorX;
			a.mgsvYawSign = Cfg().mgsvYawSign;
			a.erForward = Calibration::ErForward();
			a.mgsvCamForward = Cfg().mgsvCamForward;
			return a;
		}

		// The cast_ray filters for Snake's body (docs/HOST.md "Collision filters"): [Puppet] ground_filter for the vertical rays
		// (ground, landing, head room), wall_filter for the horizontal ones (walls; 0 = ray_filter) and wall_filter_also for them
		// too (0 = none; the nearer hit is kept). Anything but ray_filter needs the layer table this was RE'd from
		// (A::CollisionLayers): without it, ray_filter alone (logged once).
		struct BodyFilters
		{
			std::uint32_t ground;
			std::uint32_t wall;
			std::uint32_t wallAlso;  // 0 = none
			std::uint32_t keep{ 0 }; // material classes the ground and wallAlso rays keep ([Puppet] ground_keep; game::CastRayKeep)
			std::uint8_t  layers{ game::kCastRayLayers };  // ... and the broad-phase layers they look in ([Puppet] ground_layers)
		};

		BodyFilters Filters()
		{
			const auto& cfg = Cfg();
			BodyFilters f{ cfg.groundFilter ? cfg.groundFilter : cfg.rayFilter, cfg.wallFilter ? cfg.wallFilter : cfg.rayFilter, cfg.wallFilterAlso,
				cfg.groundKeep, static_cast<std::uint8_t>(cfg.groundLayers) };
			if ((f.keep || f.layers != game::kCastRayLayers) && !game::CanCastRayKeep()) {
				static bool keepWarned = false;
				if (!keepWarned) {
					keepWarned = true;
					Log::Warn("puppet: [Puppet] ground_keep {:#x} / ground_layers {:#04x} need Snake's own ray query (cast_ray's query, the snipe "
					          "collector, the world cast, the material lookup), unverified on this exe -> the game's cast_ray (wooden walkways "
					          "stay invisible to Snake)",
						f.keep, f.layers);
				}
				f.keep = 0;
				f.layers = game::kCastRayLayers;
			}
			if (f.wallAlso == f.wall) {
				f.wallAlso = 0;
			}
			static bool warned = false;
			if ((f.ground != cfg.rayFilter || f.wall != cfg.rayFilter || f.wallAlso) && !Addr().Has(A::CollisionLayers)) {
				if (!warned) {
					warned = true;
					Log::Warn("puppet: [Puppet] ground_filter {:#x} / wall_filter {:#x} / wall_filter_also {:#x} need the collision layer table, "
					          "unverified on this exe -> ray_filter {:#x} alone for Snake's body",
						f.ground, f.wall, f.wallAlso, cfg.rayFilter);
				}
				f = { cfg.rayFilter, cfg.rayFilter, 0, f.keep, f.layers };
			}
			return f;
		}

		// One body ray through a filter: the game's cast_ray, or with material classes kept (ground and wallAlso rays).
		bool BodyCast(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, std::uint32_t a_filter, std::uint32_t a_keep,
			std::uint8_t a_layers)
		{
			return a_keep || a_layers != game::kCastRayLayers ? game::CastRayKeep(a_from, a_delta, a_hit, a_ignore, a_filter, a_keep, a_layers)
			                                                  : game::CastRay(a_from, a_delta, a_hit, a_ignore, a_filter);
		}

		// Snake's body against Elden Ring's world (er/Kinematic.h BodyRays): a ray mostly up or down is a ground/head-room probe
		// (ground_filter), the others are walls (wall_filter, and wall_filter_also: the characters' own walls -- a scaffold's
		// railings in the layers whose floor only ground_filter sees; the nearer hit).
		RayFn BodyRay(const std::uint8_t* a_ignore)
		{
			const BodyFilters f = Filters();
			const auto        cast = [a_ignore](std::uint32_t a_filter, std::uint32_t a_keep, std::uint8_t a_layers) -> RayFn {
				return [a_ignore, a_filter, a_keep, a_layers](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
					return BodyCast(a_from, a_delta, a_hit, a_ignore, a_filter, a_keep, a_layers);
				};
			};
			// The wall_filter ray (ray_filter: enemies' hit capsules) stays the game's; the characters' own layer looks where the
			// game's cast_ray doesn't (the broad-phase layer of Stormveil's wooden walkways, the materials its collector drops), for
			// the ground and the walls alike (a walkway's railings too).
			return BodyRays(cast(f.ground, f.keep, f.layers), cast(f.wall, 0, game::kCastRayLayers),
				f.wallAlso ? cast(f.wallAlso, f.keep, f.layers) : RayFn{});
		}

		// What is below a_at (3 m) through a filter: "+0.12 m" / "none".
		std::string Below(const Vec3& a_at, const std::uint8_t* a_ignore, std::uint32_t a_filter, std::uint32_t a_keep = 0,
			std::uint8_t a_layers = game::kCastRayLayers)
		{
			Vec3 hit;
			return BodyCast(a_at + Vec3{ 0.0f, 0.5f, 0.0f }, Vec3{ 0.0f, -3.5f, 0.0f }, hit, a_ignore, a_filter, a_keep, a_layers)
			           ? std::format("{:+.2f} m", hit.y - a_at.y)
			           : std::string("none");
		}

		Vec3 GuestRoot(const proto::GuestState& a_g)
		{
			return { a_g.rootPos[0], a_g.rootPos[1] - Cfg().mgsvRootOffsetY, a_g.rootPos[2] };
		}

		// Snake's odometer, its epoch and his yaw for this Elden Ring frame: the paced blend of MGSV's frames around the
		// moment this frame shows (er/Pacing.h), else the newest MGSV frame.
		struct MotionIn
		{
			double        odo[3]{};
			std::uint32_t epoch{ 0 };
			float         rootYaw{ 0 };
		};

		MotionIn MotionOf(const Frame& a_f)
		{
			MotionIn    r;
			const auto& m = Pacing::Current();
			if (m.valid && m.epoch == a_f.guest.odoEpoch) {
				std::copy(std::begin(m.odo), std::end(m.odo), r.odo);
				r.epoch = m.epoch;
				r.rootYaw = m.rootYaw;
			} else {
				std::copy(std::begin(a_f.guest.odo), std::end(a_f.guest.odo), r.odo);
				r.epoch = a_f.guest.odoEpoch;
				r.rootYaw = a_f.guest.rootYaw;
			}
			return r;
		}

		void PushHost(proto::HostEventType a_type, std::uint32_t a_a, std::uint32_t a_b, const Vec3& a_v0, std::uint64_t a_frame)
		{
			proto::HostEvent e{};
			e.type = a_type;
			e.a = a_a;
			e.b = a_b;
			e.frame = static_cast<std::uint32_t>(a_frame);
			e.v0[0] = a_v0.x;
			e.v0[1] = a_v0.y;
			e.v0[2] = a_v0.z;
			if (!Link::Get().PushHostEvent(e)) {
				Log::Debug("host event {} dropped (guest ring full or detached)", static_cast<std::uint32_t>(a_type));
			}
		}

		// Snake's height in MGSV's stance (GuestState flags / stance: 0 stand, 1 squat, 2 crawl).
		float StanceHeight(const proto::GuestState& a_g)
		{
			const auto& c = Cfg();
			if ((a_g.flags & proto::kGuestProne) || a_g.stance == 2) {
				return c.heightProne;
			}
			if ((a_g.flags & proto::kGuestCrouching) || a_g.stance == 1) {
				return c.heightCrouch;
			}
			return c.heightStand;
		}
		const char* StanceName(const proto::GuestState& a_g)
		{
			if ((a_g.flags & proto::kGuestProne) || a_g.stance == 2) {
				return "prone";
			}
			if ((a_g.flags & proto::kGuestCrouching) || a_g.stance == 1) {
				return "crouched";
			}
			return "standing";
		}

		void Hide(const game::Player& a_p, bool a_hide)
		{
			const auto& mode = Cfg().hideMode;
			if (mode == "render_flag") {
				// ChrIns+0x1C5 bit3 = enable_render (fromsoftware-rs; the game toggles it around summon
				// warps at 0x4D7FDD/0x4D817B): hides the body and armour, keeps the hitbox (VERIFIED live) -- but not the
				// weapon in his hand (live: his longsword, then a spear, beside Snake; the weapons are CSPartsModelIns of
				// PlayerIns+0x648 CSChrAsmModelIns). fade, the render group bit 0x1C4 & 8 and max_render_range (+0x408)
				// leave them. His base transparency (ChrIns+0x24C, fromsoftware-rs `base_transparency`; 1 here) at 0 takes the
				// weapons with him (VERIFIED live 2026-10-12: the Tarnished and his longsword both gone, the world drawn as
				// before), so both are held while he is hidden (session 4: "a sword or spear at Snake's feet").
				game::SetBit(a_p.chr + game::kChrFlags1C5, 0x08, a_hide ? false : g_saved.renderBit);
				const float base = a_hide ? 0.0f : g_saved.baseTransparency;
				if (SafeRead<float>(a_p.chr + game::kChrBaseTransparency).value_or(base) != base) {
					SafeWrite<float>(a_p.chr + game::kChrBaseTransparency, base);
				}
			} else if (mode == "fade") {
				SafeWrite<float>(a_p.chr + game::kChrTransparencyMod, a_hide ? -1.0f : g_saved.transparencyMod);
			}
		}

		void ApplyHealthFlags(const game::Player& a_p, bool a_on)
		{
			auto* flags = a_p.data + game::kDataDebug;
			if (!a_on) {
				SafeWrite<std::uint8_t>(flags, g_saved.dataDebug);
				return;
			}
			std::uint8_t v = SafeRead<std::uint8_t>(flags).value_or(g_saved.dataDebug);
			v = static_cast<std::uint8_t>(Cfg().noDead ? (v | 0x01) : (v & ~0x01));
			v = static_cast<std::uint8_t>(Cfg().noDamage ? (v | 0x02) : (v & ~0x02));
			SafeWrite<std::uint8_t>(flags, v);
		}

		void Start(const Frame& a_f)
		{
			const auto& p = a_f.player;
			const auto& g = a_f.guest;
			g_chr = p.chr;
			g_anchor.axes = CurrentAxes();
			g_anchor.pe = game::Position(p);
			g_anchor.pm = GuestRoot(g);
			const auto motion = MotionOf(a_f);
			g_anchor.delta = ChooseDelta(game::Orientation(p), motion.rootYaw, g_anchor.axes);
			std::copy(std::begin(motion.odo), std::end(motion.odo), g_odoLast);
			g_odoEpoch = motion.epoch;
			g_kin = {};
			g_kin.pos = g_anchor.pe;
			g_kin.grounded = true;
			g_lastSet = g_anchor.pe;
			g_rmDebt = g_rmAsked = g_rmHanded = {};
			g_unstick.Reset();
			g_fallWatch.Reset();
			g_dive.Reset();
			g_jumpEdge.Reset((g.flags & proto::kGuestJumping) != 0);  // a jump under way when the puppet starts is not one to make
			g_runVel = {};
			if (!(g_hiddenForDeath && g_deathChr == p.chr)) {  // else still hidden by us: g_saved has the originals
				g_saved.renderBit = game::GetBit(p.chr + game::kChrFlags1C5, 0x08);
				g_saved.transparencyMod = SafeRead<float>(p.chr + game::kChrTransparencyMod).value_or(0.0f);
				// A sane opacity (a Mimic's Veil or a fade could be mid-way): put back as 1 if it isn't one.
				const float base = SafeRead<float>(p.chr + game::kChrBaseTransparency).value_or(1.0f);
				g_saved.baseTransparency = std::isfinite(base) && base > 0.0f && base <= 1.0f ? base : 1.0f;
			}
			g_hiddenForDeath = false;
			g_deathAt = 0;
			g_saved.dataDebug = SafeRead<std::uint8_t>(p.data + game::kDataDebug).value_or(0);
			Hide(p, true);
			ApplyHealthFlags(p, true);
			g_rootMotion = Cfg().motion == "root_motion" && RootMotion::Install(p);
			g_rootMotionSince = ::GetTickCount64();
			if (!g_rootMotion) {
				// The kinematic body stands on the ground under its centre; Elden Ring's capsule may stand on a rim with its centre
				// over a drop (live: off a ladder at the top of Fort Haight's tower Snake fell 19 m down its shaft): start on ground.
				KinematicParams kp;
				kp.radius = Cfg().radius;
				kp.stepUp = Cfg().stepUp;
				kp.snapDown = Cfg().snapDown;
				kp.chestHeight = Cfg().chestHeight;
				kp.groundProbes = Cfg().groundProbes;
				const RayFn ray = BodyRay(p.chr);
				const BodyFilters f = Filters();
				Log::Info("puppet: Snake's body rays: ground_filter {:#x}, wall_filter {:#x} + {:#x}, {}, {}; below the start: {} (ground_filter), {} "
				          "(ray_filter {:#x})",
					f.ground, f.wall, f.wallAlso, kp.groundProbes ? "ground under the whole body (5 probes)" : "ground under the centre (1 ray)",
					f.keep || f.layers != game::kCastRayLayers
						? std::format("Snake's own ray query (broad-phase layers {:#04x} of masks {}, material classes {:#x} kept)", f.layers,
							  game::BroadPhaseLayers(), f.keep)
						: std::string("the game's cast_ray"),
					Below(g_anchor.pe, p.chr, f.ground, f.keep, f.layers), Below(g_anchor.pe, p.chr, Cfg().rayFilter), Cfg().rayFilter);
				if (const auto at = SupportedStart(g_anchor.pe, kp, ray)) {
					Log::Info("puppet: no ground under the Tarnished's centre at ({:.2f}, {:.2f}, {:.2f}) (a rim, a ladder's top) -> Snake starts "
					          "{:.2f} m away on ground",
						g_anchor.pe.x, g_anchor.pe.y, g_anchor.pe.z, LengthXZ(*at - g_anchor.pe));
					game::SetPosition(p, *at);
					g_anchor.pe = *at;
					g_kin.pos = *at;
					g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
					g_kin.underRefusedS = 0;
					g_lastSet = *at;
				}
			}
			if (Cfg().hitHook && HitHook::Install(p)) {
				HitHook::SetCancel(true);
			}
			g_hpLast = game::Hp(p);
			g_maxHpLast = game::MaxHp(p);
			g_healCarry = 0;
			g_killedByGuest = false;
			g_frames = 0;
			g_dropped = 0;
			g_active = true;
			Log::Info("puppet ON ({} motion): delta {:.3f} rad, ER feet ({:.2f}, {:.2f}, {:.2f}), MGSV root ({:.2f}, {:.2f}, {:.2f}) yaw {:.3f}; {}",
				g_rootMotion ? "root" : "kinematic", g_anchor.delta, g_anchor.pe.x, g_anchor.pe.y, g_anchor.pe.z, g_anchor.pm.x, g_anchor.pm.y, g_anchor.pm.z,
				g.rootYaw, Calibration::Describe());
		}

		void Stop(const Frame& a_f, std::string_view a_why)
		{
			if (!g_active) {
				return;
			}
			g_active = false;
			// Restore only on the same, still valid character; a swapped/unloaded one is the game's (its
			// objects may be gone: the hooks just forget them, their vtable copies stay valid).
			const bool same = a_f.player && a_f.player.chr == g_chr;
			RootMotion::Remove(same ? a_f.player : game::Player{});
			HitHook::Remove(same ? a_f.player : game::Player{});
			Combat::OnPuppetStop();  // patched bullet/AtkParam rows go back before ER has the controller
			g_rootMotion = false;
			const bool dying = a_why == "player dead";
			if (dying && !g_deathAt) {
				g_deathAt = ::GetTickCount64();
				g_deathChr = g_chr;
				g_deathLoad = false;
				g_aliveSince = 0;
				g_deathPosValid = false;
			}
			if (same) {
				if (dying) {
					g_hiddenForDeath = true;  // until the respawn (AfterPhysics) or the next puppet start
				} else {
					Hide(a_f.player, false);
				}
				if (!g_killedByGuest) {
					ApplyHealthFlags(a_f.player, false);
				}
			}
			g_chr = nullptr;
			Log::Info("puppet OFF ({}) after {} frames; ER has the controller again", a_why, g_frames);
		}

		proto::HurtKind KindByDistance(float a_distance) { return a_distance < 4.0f ? proto::kHurtMelee : proto::kHurtProjectile; }

		// Elden Ring damage on the puppet -> kHEvHurt amount ([Combat] hurt_share / hurt_mult).
		std::uint32_t ToGuest(std::int32_t a_damage, const Frame& a_f)
		{
			const auto& c = Cfg();
			return combat::HurtForGuest(a_damage, game::MaxHp(a_f.player), a_f.guest.lifeMax, c.guestLifeScale, c.hurtShare, c.hurtMult);
		}

		// Where the last hit came from, as a direction in MGSV world, and what kind it probably was.
		std::pair<Vec3, proto::HurtKind> HitSource(const game::Player& a_p)
		{
			const auto attacker = game::LastHitBy(a_p);
			const Vec3 me = game::Position(a_p);
			if (g_landingSpeed > 8.0f) {
				return { Vec3{ 0, -1, 0 }, proto::kHurtFall };
			}
			for (const auto& c : game::Characters(64)) {
				if (c.handle == attacker && c.chr != a_p.chr && attacker != 0) {
					Vec3        to = c.pos - me;
					const float d = LengthXZ(to);
					to.y = 0;
					const Vec3 dir = Normalize(g_anchor.DirToMgsv(to));
					return { dir, KindByDistance(d) };
				}
			}
			return { Vec3{}, proto::kHurtOther };
		}

		// A hard landing as the host measured it (kinematic motion moves the puppet with SetPosition, so
		// Elden Ring's own fall damage may never trigger): the share of Snake's life ER would have taken.
		void LandingDamage(const Frame& a_f, float a_speed)
		{
			const auto& cfg = Cfg();
			const bool  host = cfg.fallDamage == "host" || (cfg.fallDamage == "auto" && !g_rootMotion);
			if (!host || !a_f.guestReady) {
				return;
			}
			const float fraction = combat::FallFraction(a_speed, cfg.fallHurtSpeed, cfg.fallLethalSpeed);
			const auto  amount = combat::FallHurtAmount(fraction, a_f.guest.lifeMax, cfg.guestLifeScale);
			if (amount == 0) {
				return;
			}
			PushHost(proto::kHEvHurt, amount, proto::kHurtFall, Vec3{ 0, -1, 0 }, a_f.index);
			g_fallSentFrame = a_f.index;
			++g_falls;
			Log::Info("puppet: landed at {:.1f} m/s -> fall damage {:.0f}% of Snake's life (kHEvHurt {}{})", a_speed, fraction * 100.0f, amount,
				fraction >= 1.0f ? ", lethal" : "");
		}

		// Snake's death becomes the Tarnished's: drop the guards and let Elden Ring run YOU DIED and the
		// grace respawn (verified a moment later in AfterPhysics).
		void KillForGuest(const Frame& a_f)
		{
			g_killedByGuest = true;
			g_killConfirmed = false;
			g_deathPending = false;
			g_killAt = ::GetTickCount64();
			if (!g_deathAt) {
				g_deathAt = g_killAt;
				g_deathChr = a_f.player.chr;
				g_deathLoad = false;
				g_aliveSince = 0;
				g_deathPosValid = false;
			}
			++g_killTries;
			HitHook::SetCancel(false);
			auto*      flags = a_f.player.data + game::kDataDebug;
			const auto v = SafeRead<std::uint8_t>(flags).value_or(0);
			SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(v & ~0x03));
			if (game::Kill(a_f.player)) {
				Log::Info("puppet: killed the ER player (ChrIns::Kill) for Snake's death{}", g_killTries > 1 ? std::format(" (try {})", g_killTries) : "");
			} else {
				Log::Warn("puppet: ChrIns::Kill unverified -- dropping HP to 0 instead");
				game::SetHp(a_f.player, 0);
			}
			// Restore the original debug byte except the guards we just dropped.
			g_saved.dataDebug = static_cast<std::uint8_t>(g_saved.dataDebug & ~0x03);
		}
	}

	namespace
	{
		// Dive = hop (roadmap 2.9): kGuestDiving's rising edge, then the dive's first move gives the hop its direction.
		void DiveHop(const Frame& a_f, const Vec3& a_step, const KinematicParams& a_params, const RayFn& a_ray)
		{
			const auto& cfg = Cfg();
			Vec3        dir;
			const auto  out = g_dive.Update((a_f.guest.flags & proto::kGuestDiving) != 0, cfg.hop && !g_riding, ::GetTickCount64(), a_step, a_f.dt,
				 cfg.hopWaitMs, &dir);
			if (out == DiveTrigger::Out::kNoMove) {
				if (++g_hopsNoMove <= 5) {
					Log::Info("puppet: MGSV dived but Snake didn't move within {} ms -- no hop", cfg.hopWaitMs);
				}
				return;
			}
			if (out != DiveTrigger::Out::kHop) {
				return;
			}
			HopParams hop;
			hop.height = cfg.hopHeight;
			hop.length = cfg.hopLength;
			hop.step = cfg.hopStep;
			hop.minRise = cfg.hopMinRise;
			// His standing height, not this frame's stance: the guest arms a dive only on X while STAND, and the frame that
			// confirms it may already say prone (a 0.5 m head would let a full hop take him under a 1.9 m beam).
			hop.head = cfg.ceiling ? cfg.heightStand : 0.0f;
			const bool  grounded = g_kin.grounded;
			const float airS = g_kin.airS, below = g_kin.groundY - g_kin.pos.y;
			if (g_kin.jumping) {
				++g_hopsRefused;
				Log::Info("puppet: dive -- no hop (in a jump; refused #{})", g_hopsRefused);
				return;
			}
			const float rise = KinematicHop(g_kin, dir, hop, a_params, a_ray);
			if (rise <= 0.0f) {
				++g_hopsRefused;
				Log::Info("puppet: dive -- no hop ({}less than {:.2f} m of room above his {:.2f} m: a ceiling; refused #{})",
					grounded ? std::string() : std::format("off the ground {:.2f} s and {:.2f} m (a hop from the air only within {:.2f} s and {:.2f} m), or ", airS, below,
						hop.graceS, hop.graceDrop),
					cfg.hopMinRise, hop.head > 0.0f ? hop.head : a_params.headHeight, g_hopsRefused);
				return;
			}
			++g_hops;
			Log::Info("puppet: dive -> hop #{}{}: {:.2f} m up{} toward ({:.2f}, {:.2f}) at {:.2f} m/s from ({:.2f}, {:.2f}, {:.2f})", g_hops,
				grounded ? std::string() : std::format(" (off a rim {:.2f} s ago, {:.2f} m below it)", airS, below), rise,
				rise + 1e-3f < cfg.hopHeight ? " (lower: a ceiling)" : "", dir.x, dir.z, LengthXZ(g_kin.hopVel), g_kin.pos.x, g_kin.pos.y, g_kin.pos.z);
		}

		// The guest's kGuestJumping went up: the puppet jumps (er/Kinematic.h KinematicJump) if Snake stands on the ground in
		// Elden Ring with room over his head; the run's velocity carries on in the air.
		void TryJump(const proto::GuestState& a_g, const KinematicParams& a_params, const RayFn& a_ray)
		{
			const auto& cfg = Cfg();
			const char* why = nullptr;
			if (!cfg.jump) {
				why = "[Puppet] jump = false";
			} else if (g_riding) {
				why = "riding";
			} else if ((a_g.flags & (proto::kGuestCrouching | proto::kGuestProne)) || a_g.stance != 0) {
				why = "not standing";
			} else if (!g_kin.grounded) {
				why = "already in the air";
			}
			JumpStart j;
			if (!why) {
				j = KinematicJump(g_kin, g_runVel, cfg.jumpApex, cfg.jumpMinRoom, cfg.maxSpeed, a_params, a_ray);
				why = j.ok ? nullptr : j.why;
			}
			if (!why) {
				++g_jumps;
				if (g_jumps <= 10 || g_jumps % 50 == 0) {
					Log::Info("puppet: Snake jumps -- {:.2f} m up at {:.1f} m/s{} (jump #{})", j.apex, j.speed,
						std::isfinite(j.room) ? std::format(", {:.2f} m over his head", j.room) : std::string(), g_jumps);
				}
				return;
			}
			++g_jumpsRefused;
			if (g_jumpsRefused <= 10 || g_jumpsRefused % 50 == 0) {
				Log::Info("puppet: no jump -- {}{} (refused #{})", why, std::isfinite(j.room) ? std::format(" ({:.2f} m over his head)", j.room) : std::string(),
					g_jumpsRefused);
			}
		}

		// FallWatch fired: Snake dies of the fall as he would have on landing (kHEvHurt lethal, then Elden Ring's death).
		void EndlessFall(const Frame& a_f)
		{
			++g_endlessFalls;
			Log::Info("puppet: falling {:.1f} s, {:.0f} m with no landing (a chasm: under NoDead Elden Ring's kill volume can't kill the puppet) -> Snake "
			          "dies of the fall (fall #{})",
				g_fallWatch.Seconds(), g_fallWatch.Drop(), g_endlessFalls);
			if (a_f.guestReady) {
				const auto& cfg = Cfg();
				PushHost(proto::kHEvHurt, combat::FallHurtAmount(1.0f, a_f.guest.lifeMax, cfg.guestLifeScale), proto::kHurtFall, Vec3{ 0, -1, 0 }, a_f.index);
				g_fallSentFrame = a_f.index;
			}
			if (Cfg().killOnGuestDeath && !g_killedByGuest) {
				KillForGuest(a_f);
			}
		}
	}

	bool Usable(std::string* a_why)
	{
		const auto& a = Addr();
		const char* missing = nullptr;
		if (!a.HasAll(A::ChrInsSetPosition, A::ChrInsSetYaw)) {
			missing = "SetPosition/SetYaw unverified";
		} else if (!a.HasAll(A::CastRay, A::CSHavokMan, A::VtPhysWorld, A::VtHavokManImp)) {
			missing = "ray cast unverified";
		} else if (!a.HasAll(A::WorldChrMan, A::VtPlayerIns, A::VtPhysics, A::VtData, A::VtWorldChrManImp) || !(a.Has(A::VtPlayerCtrl) || a.Has(A::VtChrCtrl))) {
			missing = "player access unverified";
		}
		if (a_why && missing) {
			*a_why = missing;
		}
		return missing == nullptr;
	}

	void Decide(const Frame& a_f)
	{
		if (g_deathAt && (!a_f.inGame || !a_f.player || a_f.player.chr != g_deathChr)) {
			// The respawn load is under way (AfterPhysics declares the respawn once the player lives again).
			g_deathLoad = true;
			if (a_f.player && a_f.player.chr != g_deathChr) {
				// A different character came (load, title): it is Elden Ring's, visible as it made it. Not on a player
				// that is merely absent: MainPlayer() can drop out for frames of the death load, and when the same chr
				// returns, AfterPhysics must still un-hide it (or Start keeps its g_saved).
				g_hiddenForDeath = false;
			}
		}
		if (!g_active && !g_deathAt && a_f.player && a_f.inGame && a_f.dead) {
			// The Tarnished died while Snake wasn't playing (MGSV booting or restarting: the player plays him meanwhile): the same
			// respawn rule as for Snake's own deaths. Before, the dead bit clearing ~6 s after YOU DIED was taken for the
			// respawn and the puppet started under "Choose revival location" (live 2026-10-12: Snake's HUD over the dialog).
			g_deathAt = ::GetTickCount64();
			g_deathChr = a_f.player.chr;
			g_deathLoad = false;
			g_aliveSince = 0;
			g_deathPosValid = false;
			Log::Info("puppet: the Tarnished died while Snake wasn't playing -- Snake takes over after his respawn");
		}
		// Not while dying: the dead bit clears seconds before Elden Ring's respawn, and a puppet started then would
		// stand Snake up (kHEvRespawn) in the middle of the death fade.
		const bool playerOk = a_f.player && a_f.inGame && !a_f.dead && !g_deathAt;
		const bool allowed = Cfg().puppet && g_holds == 0 && Usable();
		if (g_active) {
			std::string_view why;
			if (!a_f.guestAlive) {
				why = "guest lost";
			} else if (g_holds & static_cast<std::uint8_t>(Hold::kTarnished)) {
				why = "Tarnished mode";
			} else if (g_holds & static_cast<std::uint8_t>(Hold::kStepAside)) {
				why = "stepping aside: Elden Ring walks the Tarnished";
			} else if (g_holds & static_cast<std::uint8_t>(Hold::kLadder)) {
				why = "a ladder: Elden Ring climbs it with the Tarnished";
			} else if (!allowed) {
				why = "disabled";
			} else if (!a_f.player || a_f.player.chr != g_chr) {
				why = "player object changed";
			} else if (!a_f.inGame) {
				why = "loading";
			} else if (a_f.dead) {
				why = "player dead";
			}
			if (!why.empty()) {
				Stop(a_f, why);
			}
		}
		// Ray casts need the physics world; checked here (FrameBegin) so a missing world never
		// starts a puppet that can't collide.
		// Not under a blocking screen either (the respawn's loading screen: the player exists before ER's final warp).
		if (!g_active && allowed && playerOk && !a_f.modalMenu && a_f.guestReady && game::CanCastRay()) {
			Start(a_f);
		}
		if (g_deathPending && g_active && Cfg().killOnGuestDeath) {
			Log::Info("puppet: back on -- carrying out Snake's earlier death");
			KillForGuest(a_f);
		}
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		// Not "Elden Ring moved the player by itself": its whole world moved (32 / 40 m steps, VERIFIED live riding).
		g_lastSet += a_delta;
		KinematicShift(g_kin, a_delta);  // the place, and a jump's takeoff height (its landing's fall damage)
		g_unstick.Shift(a_delta);
		g_fallWatch.Shift(a_delta.y);
	}

	void Move(const Frame& a_f)
	{
		if (!g_active || !a_f.player || a_f.player.chr != g_chr) {
			return;
		}
		++g_frames;
		const auto& p = a_f.player;
		const auto& cfg = Cfg();
		g_anchor.axes = CurrentAxes();

		// Elden Ring moved the character itself (grace, elevator, cutscene warp, being shoved): follow.
		const Vec3 now = game::Position(p);
		if (Length(now - g_lastSet) > cfg.resyncDistance) {
			Log::Info("puppet: ER moved the player {:.2f} m by itself; re-anchoring", Length(now - g_lastSet));
			g_kin.pos = now;
			g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
			g_kin.underRefusedS = 0;
			g_kin.grounded = true;
			g_kin.vy = 0;
			RootMotion::Clear();
			g_rmDebt = g_rmAsked = g_rmHanded = {};
			g_fallWatch.Reset();
			PushHost(proto::kHEvWarped, 0, 0, Vec3{}, a_f.index);
		}
		if (g_rootMotion) {
			g_lastSet = now;  // ER moves the puppet itself: compare against where it was last frame
		}

		// A jump starts on the rising edge of the guest's kGuestJumping (the edge is taken whatever happens to it below: a press
		// during a cutscene is never replayed afterwards). The flag is remembered on every frame the state was read, ready or not:
		// one frame that read not ready mid-jump never makes the same press a second jump (er/Kinematic.h JumpEdge).
		const bool jumpEdge = g_jumpEdge.Update(a_f.guestRead, a_f.guestReady, (a_f.guest.flags & proto::kGuestJumping) != 0);
		if (!a_f.guestReady || a_f.cutscene || a_f.interacting) {
			RootMotion::Clear();
			g_rmDebt = g_rmAsked = g_rmHanded = {};
			// MGSV loading or in its own menu, an ER cutscene, or an ER interaction (grace, pickup) owns the player: hold
			// still, and keep the odometer baseline current so nothing Snake did meanwhile is replayed afterwards.
			if (a_f.guestReady) {
				const auto motion = MotionOf(a_f);
				std::copy(std::begin(motion.odo), std::end(motion.odo), g_odoLast);
			}
			return;
		}
		const auto& g = a_f.guest;
		const auto  motion = MotionOf(a_f);
		g_riding = (g.flags & proto::kGuestOnVehicle) || (g.buddyFlags & proto::kBuddyRidden);
		// The buddy type says what he rides: the sandbox has no other vehicles, and MGSV runs one buddy at a time.
		g_horse = g_riding && g.buddyType == Buddy::kHorse;
		if (g_riding != g_wasRiding) {
			Log::Info("puppet: Snake {} (vehicle id {:#x}, buddy type {}: {}){}", g_riding ? "mounted" : "dismounted", g.vehicleId, g.buddyType,
				Buddy::TypeName(g.buddyType),
				g_riding ? std::format(" -> max {:.0f} m/s, step-up {:.2f} m, radius {:.2f} m", g_horse ? cfg.maxSpeedHorse : cfg.maxSpeedVehicle,
					g_horse ? cfg.stepUpHorse : cfg.stepUpVehicle, g_horse ? cfg.radiusHorse : cfg.radiusVehicle) : std::string());
			g_wasRiding = g_riding;
		}

		Vec3 step{};
		if (motion.epoch != g_odoEpoch) {
			Log::Info("puppet: guest odometer restarted (epoch {} -> {})", g_odoEpoch, motion.epoch);
			g_odoEpoch = motion.epoch;
		} else {
			const Vec3 d{ static_cast<float>(motion.odo[0] - g_odoLast[0]), static_cast<float>(motion.odo[1] - g_odoLast[1]), static_cast<float>(motion.odo[2] - g_odoLast[2]) };
			const float dist = LengthXZ(d);
			const float maxStep = (g_horse ? cfg.maxSpeedHorse : g_riding ? cfg.maxSpeedVehicle : cfg.maxSpeed) * std::clamp(a_f.dt, 1.0f / 240.0f, 0.1f);
			if (!Finite(d) || dist > cfg.teleportDrop) {
				if (++g_dropped <= 20) {
					Log::Warn("puppet: odometer jumped {:.2f} m in one frame; ignored (teleport protection)", dist);
				}
			} else {
				step = g_anchor.DirToEr(dist > maxStep ? d * (maxStep / dist) : d);
			}
		}
		std::copy(std::begin(motion.odo), std::end(motion.odo), g_odoLast);
		const float heading = MgsvHeading(motion.rootYaw, g_anchor.axes) + g_anchor.delta;

		if (g_rootMotion && !RootMotion::Alive(1000.0) && ::GetTickCount64() - g_rootMotionSince > 2000) {
			Log::Warn("puppet: the root-motion hook has not run for 1 s ({}); switching to kinematic motion", RootMotion::Describe());
			RootMotion::Remove(p);
			g_rootMotion = false;
			g_kin.pos = now;
			g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
			g_kin.underRefusedS = 0;
			g_kin.grounded = true;
			g_kin.vy = 0;
			g_lastSet = now;
		}
		if (g_rootMotion && jumpEdge && !g_jumpRootWarned) {
			g_jumpRootWarned = true;
			Log::Info("puppet: Snake jumped in MGSV, but jumps need [Puppet] motion = kinematic (Elden Ring's proxy moves him now) -- no jump");
		}
		if (g_rootMotion) {
			// Elden Ring's proxy moves the puppet during this frame's physics; our state just mirrors it.
			Vec3 moved = now - g_kin.pos;
			moved.y = 0;
			g_wantedM += LengthXZ(step);
			g_movedM += LengthXZ(moved);
			// What last frame's handing achieved (the proxy moved `moved` of `g_rmHanded`): the rest is still owed, unless the
			// proxy was blocked (it moved < 30% of what it was handed: a wall, a rock) -- then the debt drains.
			const float handed = LengthXZ(g_rmHanded);
			const bool  blocked = handed > 1e-3f && Dot(moved, g_rmHanded) < 0.3f * handed * handed;
			g_wallFrames += blocked ? 1 : 0;
			g_walls += blocked && !g_wasBlocked ? 1 : 0;
			g_wasBlocked = blocked;
			if (handed > 1e-4f && Dot(moved, g_rmHanded) < 0.3f * handed * handed) {
				g_rmDebt = g_rmDebt * 0.8f;
			} else {
				g_rmDebt += g_rmAsked - moved;
			}
			const auto nowMs = ::GetTickCount64();
			if (LengthXZ(step) > 1e-4f) {
				g_rmStillSince = 0;
			} else if (!g_rmStillSince) {
				g_rmStillSince = nowMs;
			}
			if (g_rmStillSince && nowMs - g_rmStillSince > 150) {
				g_rmDebt = g_rmDebt * 0.7f;  // Snake stopped: the puppet doesn't creep on
			}
			g_rmDebt.y = 0;
			const float maxDebt = std::max(0.0f, cfg.catchUp);
			if (LengthXZ(g_rmDebt) > maxDebt) {
				g_rmDebt = LengthXZ(g_rmDebt) > 1e-6f ? g_rmDebt * (maxDebt / LengthXZ(g_rmDebt)) : Vec3{};
			}
			g_rmAsked = step;
			g_rmHanded = step + g_rmDebt * (maxDebt > 0.0f ? 0.35f : 0.0f);
			RootMotion::Add(g_rmHanded);
			const float dt = std::max(a_f.dt, 1.0f / 240.0f);
			const float vy = (now.y - g_kin.pos.y) / dt;
			bool        grounded = vy > -3.0f;
			if (!grounded) {
				// A fast descent on ground is no fall (review: a downhill gallop at 12-15 m/s on a 15-degree slope, a lift going down
				// would set off the fall watchdog after 4 s, and the flat at the bottom read as a landing): ground right under the
				// proxy says so. A real fall reads grounded only in its last metres (the landing comes that much early).
				Vec3 hit;
				const BodyFilters f = Filters();
				grounded = BodyCast(now + Vec3{ 0.0f, 0.5f, 0.0f }, Vec3{ 0.0f, g_riding ? -3.0f : -2.0f, 0.0f }, hit, p.chr, f.ground, f.keep, f.layers);
			}
			if (grounded && !g_kin.grounded) {
				g_landingSpeed = std::max(0.0f, -g_kin.vy);
				LandingDamage(a_f, g_landingSpeed);
			} else {
				g_landingSpeed = std::max(0.0f, g_landingSpeed - 20.0f * a_f.dt);
			}
			g_kin.vy = vy;
			g_kin.grounded = grounded;
			g_kin.groundY = NAN;
			g_kin.pos = now;
			g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
			g_kin.underRefusedS = 0;
			if (g_fallWatch.Update(grounded, now.y, a_f.dt, cfg.fallWatchSeconds, cfg.fallWatchDrop)) {
				EndlessFall(a_f);
			}
			game::SetYaw(p, ErYawForHeading(heading, g_anchor.axes));
			g_anchor.pe = now;
			g_anchor.pm = GuestRoot(g);
			PacingProbe::OnMoved(a_f, now, step);
			return;
		}

		KinematicParams params;
		params.radius = g_horse ? cfg.radiusHorse : g_riding ? cfg.radiusVehicle : cfg.radius;
		params.stepUp = g_horse ? cfg.stepUpHorse : g_riding ? cfg.stepUpVehicle : cfg.stepUp;
		params.snapDown = cfg.snapDown;
		params.chestHeight = cfg.chestHeight;
		params.gravity = cfg.gravity;
		params.bodyHeight = !cfg.ceiling ? 0.0f : g_riding ? cfg.heightRide : StanceHeight(g);
		params.groundProbes = cfg.groundProbes;
		params.jumpAirStep = cfg.jumpAirStep;
		params.jumpAirControl = cfg.jumpAirControl;
		const RayFn ray = BodyRay(p.chr);
		if (jumpEdge) {
			TryJump(g, params, ray);
		}
		DiveHop(a_f, step, params, ray);
		if (g_kin.jumping) {
			step.y = 0.0f;  // in the air the puppet flies its own jump: MGSV's own lift and fall of Snake (the odometer's height) never count
		}
		const bool  hopping = g_kin.hopping;
		const bool  wasGrounded = g_kin.grounded;  // after a jump's or a hop's take-off: neither is a walk off the ground
		const auto  r = KinematicStep(g_kin, step, a_f.dt, params, ray);
		if (hopping && !g_kin.hopping) {
			Log::Debug("puppet: hop over: {} at ({:.2f}, {:.2f}, {:.2f})", r.landed ? std::format("landed at {:.1f} m/s", r.landingSpeed) : std::string("falling on"),
				g_kin.pos.x, g_kin.pos.y, g_kin.pos.z);
		}
		if (wasGrounded && !g_kin.grounded && !g_kin.jumping && !g_kin.hopping && (++g_groundLost <= 20 || g_groundLost % 50 == 0 || g_frames <= 120)) {
			// Walked off ground (not a jump, not a hop): what each filter sees below him, and what the body's five ground rays
			// see where the ground went (from 0.55 m above his feet to 3 m below), so a fall through something the Tarnished stands
			// on shows in the log (live 2026-10-14: Snake fell through Stormveil's wooden scaffolding). Nothing under any ray where
			// the Tarnished stood: the filter doesn't see that floor. The first 20, any in a start's first 2 s, then every 50th.
			const BodyFilters f = Filters();
			float             below[5];
			const Vec3        at = g_kin.pos - Vec3{ 0.0f, g_kin.vy * a_f.dt, 0.0f };  // where the ground went, not this frame's drop
			GroundProbes(at, 0.55f, 3.55f, params.radius, ray, below);
			std::string seen;
			for (const float b : below) {
				seen += std::isfinite(b) ? std::format(" {:.2f}", b) : std::string(" -");
			}
			Log::Info("puppet: Snake left the ground at ({:.2f}, {:.2f}, {:.2f}){} -- below him: {} (ground_filter {:#x}), {} (ray_filter {:#x}); his five "
			          "ground rays (centre first) down to 3 m:{} (#{})",
				at.x, at.y, at.z, g_frames <= 120 ? std::format(" {} frames after the start", g_frames) : std::string(), Below(at, p.chr, f.ground, f.keep, f.layers), f.ground,
				Below(at, p.chr, cfg.rayFilter), cfg.rayFilter, seen, g_groundLost);
		}
		if (g_kin.grounded && !r.landed) {
			// The puppet's velocity on the ground (eased over a few frames): a jump's takeoff carries it on.
			const float dt = std::clamp(a_f.dt, 1.0f / 240.0f, 0.1f);
			const Vec3  v{ r.moved.x / dt, 0.0f, r.moved.z / dt };
			g_runVel = Finite(v) ? g_runVel + (v - g_runVel) * 0.35f : Vec3{};
		}
		if (r.jumpLanded) {
			Log::Debug("puppet: the jump landed at {:.2f} m ({:+.2f} m from where it left), as a fall of {:.1f} m/s", g_kin.pos.y, g_kin.pos.y - g_kin.jumpFromY,
				r.landingSpeed);
		}
		if (r.ceilingEscape) {
			Log::Info("puppet: under a ceiling {:.2f} m up ({}: {:.2f} m) and every move refused for {:.2f} s -- the ceiling check stands aside until "
			          "he stands free (a way out that dips)",
				std::isfinite(r.headroom) ? r.headroom : 0.0f, StanceName(g), params.bodyHeight, kCeilingEscapeS);
		}
		if (r.ceiling) {
			if (++g_ceilings <= 5 || g_ceilings % 200 == 0) {
				Log::Info("puppet: a ceiling {:.2f} m above where Snake would go ({}: {:.2f} m) -- he stays out from under it ({} refused frames)",
					std::isfinite(r.headroom) ? r.headroom : 0.0f, StanceName(g), params.bodyHeight, g_ceilings);
			}
		}
		{
			const bool blocked = r.blocked && LengthXZ(step) > 1e-3f;
			g_wallFrames += blocked ? 1 : 0;
			g_walls += blocked && !g_wasBlocked ? 1 : 0;
			g_wasBlocked = blocked;
		}
		if (r.blocked && LengthXZ(step) > 1e-3f && ::GetTickCount64() - g_lastBump > 400) {
			++g_bumps;
			// Running into a crate, a pot or a cart breaks it, as the Tarnished's own body does (Elden Ring's proxy does that by
			// itself with root motion).
			g_lastBump = ::GetTickCount64();
			Explosion::Break(a_f, r.wallPoint, Normalize(step), "Snake bumped into it");
		}
		const double nowS = static_cast<double>(::GetTickCount64()) / 1000.0;
		if (cfg.unstick) {
			// Asked and refused in every direction tried (er/Kinematic.h Unstick::Stall): back to where he stood free, or a hint.
			bool noPlace = false;
			if (const auto back = g_unstick.Stall(g_kin.pos, step, r.moved, g_kin.grounded, nowS, cfg.stallSeconds, &noPlace)) {
				Log::Info("puppet: stuck at ({:.2f}, {:.2f}, {:.2f}) -- every move refused for {:.1f} s in 3+ directions -- back to ({:.2f}, {:.2f}, {:.2f}) "
				          "where he stood free (stall #{}, unstick #{})",
					g_kin.pos.x, g_kin.pos.y, g_kin.pos.z, cfg.stallSeconds, back->x, back->y, back->z, g_unstick.Stalls(), g_unstick.Count());
				Hud::Notify("Snake was stuck -- back where he stood free", Hud::Tone::kWarn);
				g_kin.pos = *back;
				g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
				g_kin.underRefusedS = 0;
				g_kin.vy = 0;
				g_kin.grounded = true;
			} else if (noPlace) {
				Log::Info("puppet: stuck at ({:.2f}, {:.2f}, {:.2f}) -- every move refused for {:.1f} s in 3+ directions, and no free place to go back to "
				          "(stall #{}): Tarnished mode suggested",
					g_kin.pos.x, g_kin.pos.y, g_kin.pos.z, cfg.stallSeconds, g_unstick.Stalls());
				Hud::Notify("Snake is stuck -- hold View+Start: the Tarnished can walk out, then View+Start again", Hud::Tone::kWarn);
			}
		}
		if (cfg.unstick && g_frames % 3 == 0) {
			const int  boxed = Enclosed(g_kin.pos, params.radius + 0.3f, params, ray);
			const bool room = params.bodyHeight <= 0.0f || SolidHeadroom(g_kin.pos, params.bodyHeight + 0.05f, ray, params.stepUp) >= params.bodyHeight;
			if (const auto back = g_unstick.Update(g_kin.pos, boxed, g_kin.grounded, nowS, cfg.unstickSeconds, room)) {
				Log::Info("puppet: boxed in at ({:.2f}, {:.2f}, {:.2f}) ({} of 8 directions blocked) for {:.1f} s -- back to ({:.2f}, {:.2f}, {:.2f}) where it "
				          "stood free (unstick #{})",
					g_kin.pos.x, g_kin.pos.y, g_kin.pos.z, boxed, cfg.unstickSeconds, back->x, back->y, back->z, g_unstick.Count());
				g_kin.pos = *back;
				g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
				g_kin.underRefusedS = 0;
				g_kin.vy = 0;
				g_kin.grounded = true;
			}
		}
		g_landingSpeed = r.landed ? r.landingSpeed : std::max(0.0f, g_landingSpeed - 20.0f * a_f.dt);
		if (r.landed && r.landingSpeed > 6.0f) {
			Log::Debug("puppet: landed at {:.1f} m/s", r.landingSpeed);
		}
		if (r.landed) {
			LandingDamage(a_f, r.landingSpeed);
		}
		if (g_fallWatch.Update(g_kin.grounded, g_kin.pos.y, a_f.dt, cfg.fallWatchSeconds, cfg.fallWatchDrop)) {
			EndlessFall(a_f);
		}

		g_wantedM += LengthXZ(step);
		g_movedM += LengthXZ(r.moved);
		game::SetPosition(p, g_kin.pos);
		g_lastSet = g_kin.pos;
		game::SetYaw(p, ErYawForHeading(heading, g_anchor.axes));

		g_anchor.pe = g_kin.pos;
		g_anchor.pm = GuestRoot(g);
		PacingProbe::OnMoved(a_f, g_kin.pos, step);
	}

	void AfterPhysics(const Frame& a_f)
	{
		// Respawn: the player lives again in the world after a death (grace / death respawn), puppeting or not.
		if (a_f.player && a_f.inGame) {
			const ULONGLONG now = ::GetTickCount64();
			if (g_hiddenForDeath && !g_active && a_f.player.chr == g_deathChr) {
				Hide(a_f.player, true);  // keep it hidden through the death (the game re-sets the bit around warps)
			}
			if (g_killedByGuest && a_f.dead) {
				g_killConfirmed = true;
			}
			bool respawned = false;
			if (g_deathAt) {
				// Not while an Elden Ring screen is up: near a Stake of Marika the dead player waits in "Choose revival
				// location" (dead bit already clear, no blocking menu, ER's HUD hidden; VERIFIED live) for as long as the
				// player likes.
				g_aliveSince = a_f.dead || a_f.menuOpen || a_f.hudHidden ? 0 : (g_aliveSince ? g_aliveSince : now);
				// Elden Ring clears the dead bit ~6 s after YOU DIED, before its respawn load (VERIFIED live): only a
				// living player after a load counts, or one alive long enough without any (er/Revival.h; with ER's HUD
				// off, the hidden HUD can't show the revival dialog, so the player must also have been moved away).
				const bool same = a_f.player.chr == g_deathChr;
				const Vec3 pos = same ? game::Position(a_f.player) : Vec3{};
				if (same && !g_deathPosValid && !g_deathLoad) {
					g_deathPos = pos;
					g_deathPosValid = true;
				}
				RevivalInput in;
				in.load = g_deathLoad;
				in.alive = g_aliveSince != 0;
				in.aliveMs = g_aliveSince ? now - g_aliveSince : 0;
				in.hudKnown = a_f.hudKnown;
				in.movedM = same && g_deathPosValid ? Length(pos - g_deathPos) : 0.0f;
				respawned = Revived(in);
			} else {
				respawned = g_wasDead && !a_f.dead;  // a death the puppet never saw (no guest then)
			}
			if (respawned) {
				Log::Info("ER player respawned{} -> kHEvRespawn, kHEvRested 1 (Snake's life and ammo refilled)",
					g_deathAt ? std::format(" ({:.1f} s after the death{})", (now - g_deathAt) / 1000.0, g_deathLoad ? ", after the load" : "") : "");
				if (g_hiddenForDeath && !g_active && a_f.player.chr == g_deathChr) {
					Hide(a_f.player, false);  // Elden Ring's again unless Snake takes over (the next puppet start hides it)
				}
				g_hiddenForDeath = false;
				g_deathAt = 0;
				g_deathLoad = false;
				g_aliveSince = 0;
				g_deathPosValid = false;
				PushHost(proto::kHEvRespawn, 0, 0, Vec3{}, a_f.index);
				// Elden Ring refilled the flasks: Snake's ammo too (protocol v10).
				PushHost(proto::kHEvRested, 1, 0, Vec3{}, a_f.index);
				Fulton::OnEnemiesBack("a respawn");
				g_killedByGuest = false;
				g_killConfirmed = false;
				g_deathPending = false;
				g_killTries = 0;
			}
			g_wasDead = a_f.dead;
			// The kill must show up as ER's dead flag within a few seconds; if it doesn't (a modifier that
			// blocks death, a guard we don't know), try again, then give up and let MGSV refill Snake so
			// the game never sits at life 1 with nobody dying.
			if (g_killedByGuest && !g_killConfirmed && !a_f.dead && now - g_killAt > 2500) {
				if (g_killTries < 3 && g_active && a_f.player.chr == g_chr) {
					Log::Warn("puppet: the ER player is still alive {} ms after Kill; retrying", ::GetTickCount64() - g_killAt);
					KillForGuest(a_f);
				} else {
					Log::Warn("puppet: Elden Ring would not kill the player ({} tries) -- refilling Snake instead (kHEvRespawn)", g_killTries);
					g_killedByGuest = false;
					g_deathAt = 0;
					g_killTries = 0;
					PushHost(proto::kHEvRespawn, 1, 0, Vec3{}, a_f.index);
				}
			}
		}
		if (!g_active || !a_f.player || a_f.player.chr != g_chr) {
			return;
		}
		const auto& p = a_f.player;
		Hide(p, true);  // re-assert: the game toggles the render bit itself around warps
		if (!g_killedByGuest) {
			ApplyHealthFlags(p, true);
		}

		SyncAnchor(a_f);

		HitHook::SetCancel(!g_killedByGuest);
		for (const auto& hit : HitHook::Drain()) {
			const auto  from = hit.attacker ? game::ChrPosition(hit.attacker) : std::nullopt;
			const float d = from ? LengthXZ(*from - g_anchor.pe) : 99.0f;
			Vec3        dir = hit.from;
			dir.y = 0;
			dir = Normalize(g_anchor.DirToMgsv(dir));
			const auto amount = ToGuest(hit.amount, a_f);
			PushHost(proto::kHEvHurt, amount, from ? KindByDistance(d) : proto::kHurtOther, dir, a_f.index);
			Log::Debug("puppet: hit hook {} HP (kind {:#x}, atk {}) from {:.1f} m -> kHEvHurt {}", hit.amount, hit.kind, hit.atkParam, d, amount);
		}

		const std::int32_t maxHp = std::max(1, game::MaxHp(p));
		const std::int32_t hp = game::Hp(p);
		const bool         maxChanged = maxHp != g_maxHpLast;  // a talisman/buff changed max HP: ER may rescale HP itself
		g_maxHpLast = maxHp;
		if (hp > g_hpLast && !g_killedByGuest && !maxChanged && Cfg().mirrorLife && Cfg().forwardHeals && a_f.guestReady) {
			// Elden Ring healed the puppet above the mirror we set last frame (Crimson Tears, a grace rest, regeneration):
			// MGSV's life is the truth, so the heal goes to Snake as the same share and comes back through the mirror.
			const auto healed = hp - g_hpLast;
			const auto amount = combat::HealForGuest(healed, maxHp, a_f.guest.lifeMax, Cfg().guestLifeScale, g_healCarry);
			if (amount) {
				PushHost(proto::kHEvHeal, amount, static_cast<std::uint32_t>(healed), Vec3{}, a_f.index);
			}
			++g_heals;
			g_healedHp += healed;
			if (healed * 50 >= maxHp) {  // regeneration ticks are debug noise; flasks and graces are worth a line
				Log::Info("puppet: Elden Ring healed {} HP ({:.0f}% of {}) -> Snake +{} life (kHEvHeal {})", healed, 100.0 * healed / maxHp, maxHp,
					static_cast<int>(amount * Cfg().guestLifeScale), amount);
			} else {
				Log::Debug("puppet: Elden Ring healed {} HP -> kHEvHeal {}", healed, amount);
			}
		}
		if (hp < g_hpLast && !g_killedByGuest) {
			const auto [dir, kind] = HitSource(p);
			if (kind == proto::kHurtFall && g_fallSentFrame && a_f.index - g_fallSentFrame < 90) {
				// Elden Ring's own fall damage for a landing we already charged Snake for.
				Log::Debug("puppet: ER fall damage {} ignored (host fall damage already sent)", g_hpLast - hp);
			} else {
				const auto amount = ToGuest(g_hpLast - hp, a_f);
				PushHost(proto::kHEvHurt, amount, kind, dir, a_f.index);
				Log::Debug("puppet: hurt {} (kind {}) -> kHEvHurt {}", g_hpLast - hp, static_cast<int>(kind), amount);
			}
		}
		// The HP bar is Snake's life: HP = max(1, round(maxHp x life / lifeMax)) (not while MGSV loads: then the last value).
		std::int32_t target = Cfg().mirrorLife ? std::clamp(g_hpLast, 1, maxHp) : maxHp;
		if (Cfg().mirrorLife && a_f.guestReady && a_f.guest.lifeMax > 0) {
			target = combat::MirrorHp(a_f.guest.life, a_f.guest.lifeMax, maxHp);
		}
		if (!g_killedByGuest) {
			game::SetHp(p, target);
			g_hpLast = target;
		}
		g_hpFrac = static_cast<float>(game::Hp(p)) / static_cast<float>(maxHp);
	}

	void SyncAnchor(const Frame& a_f)
	{
		// After Elden Ring's physics step the puppet's real position is the truth (root motion moves it
		// there; in kinematic mode it is where SetPosition put it, plus whatever the proxy resolved).
		if (g_active && a_f.player && a_f.player.chr == g_chr) {
			const Vec3 pos = game::Position(a_f.player);
			if (Finite(pos)) {
				g_anchor.pe = pos;
			}
		}
	}

	void OnGuestDied(const Frame& a_f)
	{
		Log::Info("guest: Snake died");
		if (g_killedByGuest) {
			return;
		}
		if (!Cfg().killOnGuestDeath) {
			// No ER death follows, so no kHEvRespawn would either: the guest keeps its collapse (pad
			// blocked, up to its 120 s cap) and its death latch (no later kGEvDied). Refill Snake at once,
			// as the kill-failure path does (UNVERIFIED live with this option off; the default is on).
			Log::Info("puppet: kill_on_guest_death is off -- refilling Snake (kHEvRespawn)");
			PushHost(proto::kHEvRespawn, 1, 0, Vec3{}, a_f.index);
			return;
		}
		if (!g_active || !a_f.player || a_f.player.chr != g_chr) {
			// ER menu, load, cutscene: the guest latched its death and won't say it again, so remember it.
			if (!a_f.dead) {
				g_deathPending = true;
				Log::Info("puppet: not puppeting right now -- the ER player dies as soon as the puppet is back");
			}
			return;
		}
		KillForGuest(a_f);
	}

	bool          Active() { return g_active; }
	const Anchor& CurrentAnchor() { return g_anchor; }
	bool          Riding() { return g_active && g_riding; }
	bool          RidingHorse() { return g_active && g_riding && g_horse; }
	float         GroundY() { return g_active ? g_kin.groundY : NAN; }
	float         FallSpeed() { return g_active && !g_kin.grounded ? std::max(0.0f, -g_kin.vy) : 0.0f; }
	bool          Jumping() { return g_active && !g_rootMotion && g_kin.jumping && !g_kin.grounded; }
	float         HpFrac() { return g_hpFrac; }
	float         DeathSeconds() { return g_deathAt ? static_cast<float>(::GetTickCount64() - g_deathAt) / 1000.0f : -1.0f; }
	bool          Dying() { return g_deathAt != 0; }
	void          SetOverride(Override a_mode)
	{
		SetHold(Hold::kConsole, a_mode == Override::kOff);
		if (a_mode == Override::kAuto) {
			SetHold(Hold::kStepAside, false);
		}
	}
	void SetHold(Hold a_reason, bool a_on)
	{
		const auto bit = static_cast<std::uint8_t>(a_reason);
		g_holds = static_cast<std::uint8_t>(a_on ? (g_holds | bit) : (g_holds & ~bit));
	}
	bool Held(Hold a_reason) { return (g_holds & static_cast<std::uint8_t>(a_reason)) != 0; }

	std::string SetMotion(std::string_view a_mode, const Frame& a_f)
	{
		if (a_mode != "kinematic" && a_mode != "root_motion") {
			return "puppet motion kinematic|root_motion";
		}
		Cfg().motion = std::string(a_mode);
		g_wantedM = g_movedM = 0;
		g_wallFrames = g_walls = g_bumps = g_ceilings = 0;
		if (g_active && a_f.player && a_f.player.chr == g_chr) {
			const bool want = a_mode == "root_motion";
			if (want && !g_rootMotion) {
				g_rootMotion = RootMotion::Install(a_f.player);
				g_rootMotionSince = ::GetTickCount64();
			} else if (!want && g_rootMotion) {
				RootMotion::Remove(a_f.player);
				g_rootMotion = false;
			}
			const Vec3 now = game::Position(a_f.player);
			g_kin.pos = now;
			g_kin.ceilingEscape = false;  // a new place: the ceiling check's way out (Kinematic.h) ends
			g_kin.underRefusedS = 0;
			g_kin.grounded = true;
			g_kin.vy = 0;
			g_lastSet = now;
		}
		return Describe();
	}

	std::string Describe()
	{
		if (!g_active) {
			std::string why;
			const bool  usable = Usable(&why);
			std::string held;
			if (g_holds & static_cast<std::uint8_t>(Hold::kConsole)) {
				held += "console off, ";
			}
			if (g_holds & static_cast<std::uint8_t>(Hold::kTarnished)) {
				held += "Tarnished mode, ";
			}
			if (g_holds & static_cast<std::uint8_t>(Hold::kStepAside)) {
				held += "stepping aside, ";
			}
			if (g_holds & static_cast<std::uint8_t>(Hold::kLadder)) {
				held += "on a ladder, ";
			}
			return std::format("puppet off ({}{})", held, usable ? "ready to start" : why);
		}
		return std::format("puppet ON {} frames ({} motion, moved {:.2f} of {:.2f} m asked, catch-up owed {:.2f} m, unstuck {} (stalls {}), walls {} ({} frames), "
		                   "ceilings {}, breakers {}, Snake jumped {} ({} refused), hops {} (refused {}, no move {}){}): delta {:.3f}, feet ({:.2f}, "
		                   "{:.2f}, {:.2f}) {} vy {:.2f}, ground {:.2f}, riding {}, dropped jumps {}, host falls {}, endless falls {}, HP {} (bar {:.0f}%), heals {} ({} HP){}\n{}; {}",
			g_frames, g_rootMotion ? "root" : "kinematic", g_movedM, g_wantedM, LengthXZ(g_rmDebt), g_unstick.Count(), g_unstick.Stalls(), g_walls, g_wallFrames, g_ceilings, g_bumps,
			g_jumps, g_jumpsRefused, g_hops, g_hopsRefused, g_hopsNoMove, Cfg().hop ? "" : " [hop off]", g_anchor.delta, g_kin.pos.x, g_kin.pos.y, g_kin.pos.z,
			g_kin.grounded ? "grounded" : g_kin.jumping ? "jumping" : g_kin.hopping ? "hopping" : "airborne",
			g_kin.vy, g_kin.groundY, g_riding ? (g_horse ? "D-Horse" : "a vehicle") : "no", g_dropped, g_falls, g_endlessFalls, g_hpLast, g_hpFrac * 100.0f, g_heals, g_healedHp,
			g_killedByGuest ? ", DYING for Snake" : (g_deathPending ? ", death pending" : ""),
			RootMotion::Describe(), HitHook::Describe());
	}
}
