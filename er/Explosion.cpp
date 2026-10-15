#include "Explosion.h"

#include "Addresses.h"
#include "BulletSpawn.h"
#include "CombatLogic.h"
#include "Config.h"
#include "Log.h"
#include "Mem.h"
#include "Params.h"
#include "Puppet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <numbers>

#include <windows.h>

namespace phantom::er::Explosion
{
	namespace
	{
		using explosion::Kind;
		using explosion::Mode;
		using Row = std::array<std::uint8_t, params::kBullet.rowSize>;
		using AtkRow = std::array<std::uint8_t, params::kAtkParamPc.rowSize>;

		// One look, ready to spawn: its carrier row holds the template's effect-only copy.
		struct Prepared
		{
			bool          ok{ false };
			std::uint32_t templateId{ 0 };
			std::int32_t  sfx{ -1 };
			std::uint32_t carrier{ 0 };
			float         native{ 1.0f };  // m, one burst
			float         life{ 0.0f };    // s, the burst bullet's own life (its effect may last longer)
			bool          chainKept{ false };
			bool          breaks{ false };  // carries the object-only attack
			std::string   why;             // when !ok
		};

		// CSBulletManager (fromsoftware-rs cs/bullet_manager.rs, cs/bullet_ins.rs [V-src]): the live bullets are a linked list
		// from +0x08 (BufferAndAllocLinkedList head), each CSBulletIns' next at +0xE18; its wait / fly / explosion states
		// (+0xD90 / +0xDC0 / +0xDF0, CSBulletState: +0x10 = the BulletParam row id). VERIFIED live by `boom pool`: our carrier
		// ids appear there while their bursts play.
		constexpr std::uint32_t kListHead = 0x08;
		constexpr std::uint32_t kNextBullet = 0xE18;
		constexpr std::uint32_t kStateIds[] = { 0xDD0, 0xDA0, 0xE00 };
		constexpr int           kListMax = 600;

		params::Table                           g_bullet{ params::kBullet };
		params::Table                           g_atk{ params::kAtkParamPc };
		params::Patch                           g_patch;
		std::array<Prepared, explosion::kKinds> g_prep;
		// The object-only attacks: [0] light (shots, strikes, bumps: atk_obj 10, what breaks a pot, a crate, a bush) and [1] heavy
		// (blasts: atk_obj 100, meant for the defence-90/95 assets too; live, a defence-90 Limgrave fence (AEG700_013) stayed up
		// even at atk_obj 999 -- they need something else, UNVERIFIED what; every defence-0 one in reach breaks).
		struct ObjAttack
		{
			bool          ok{ false };
			std::uint32_t id{ 0 };
			int           power{ 0 };
			std::string   why;
		};
		std::array<ObjAttack, 2> g_objAtk;
		explosion::CloudGate                    g_gate;
		combat::RateLimiter                     g_limiter{ 40.0f, 20.0f };
		combat::RateLimiter                     g_breakLimiter{ 20.0f, 10.0f };
		combat::RateLimiter                     g_flameLimiter{ 6.0f, 3.0f };
		combat::RateLimiter                     g_muzzleLimiter{ 10.0f, 3.0f };
		std::uint64_t                           g_muzzles{ 0 }, g_muzzleDropped{ 0 };
		float                                   g_muzzleLife{ -1.0f };  // the life the muzzle carrier was written with
		std::string                             g_modeOverride;  // console: "" = ini
		std::string                             g_lastOffReason;
		ULONGLONG                               g_lastSpawn{ 0 };
		float                                   g_maxLife{ 0.0f };
		std::uint64_t g_blasts{ 0 }, g_bursts{ 0 }, g_refused{ 0 }, g_gated{ 0 }, g_off{ 0 }, g_logged{ 0 }, g_capped{ 0 }, g_breaks{ 0 }, g_flames{ 0 };
		// Elden Ring refusing our spawns (error 4: its pool is full): a run of refusals is warned about (first one, then every
		// 10 s) whatever the log budget, and a cloud is not refreshed again until a spawn went through or kRefusalBackoffMs
		// passed (session 4: a smoke cloud re-blasted 7 bursts every 2.5 s into a pool that refused them for a minute).
		std::uint64_t g_refusedRun{ 0 }, g_backedOff{ 0 };
		ULONGLONG     g_lastRefusal{ 0 }, g_lastRefusalWarn{ 0 };
		constexpr ULONGLONG kRefusalBackoffMs = 10000;
		std::array<std::uint64_t, explosion::kKinds> g_byKind{};
		std::uint32_t                                g_phase{ 0 };

		// Our bullets alive in Elden Ring's pool, by kind (counted once per frame, on demand).
		struct Live
		{
			std::array<int, explosion::kKinds> byKind{};
			int                                total{ 0 };    // ours
			int                                listed{ 0 };   // everything in the list
			int                                emitters{ 0 }; // CSBulletManager +0x1D4: emitters alive (of 256: er/BulletSpawn.h)
			std::uint64_t                      frame{ ~0ull };
		};
		Live g_live;
		int  g_spawnedThisFrame{ 0 };

		// How long a burst's effect may still be on screen after its bullet is gone (the fireball outlives its
		// 0.07 s bullet): the carrier bytes wait this long past the last spawn before going back.
		constexpr float kEffectTail = 8.0f;

		template <class... Args>
		void BoomLog(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (++g_logged <= 60) {
				Log::Info(a_fmt, std::forward<Args>(a_args)...);
			} else {
				Log::Debug(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		std::string_view Configured() { return g_modeOverride.empty() ? std::string_view(Cfg().explosionFx) : std::string_view(g_modeOverride); }

		// The mode in force and, when it is off, why.
		Mode Effective(std::string& a_why)
		{
			const auto mode = explosion::ParseMode(Configured()).value_or(Mode::kAuto);
			if (mode == Mode::kOff) {
				a_why = "explosion_fx = off";
				return Mode::kOff;
			}
			if (mode == Mode::kSfx) {
				a_why = "explosion_fx = sfx: no verified Elden Ring effect-spawn call in this build (none in fromsoftware-rs or the CT; "
				        "use bullet, which plays the pots' own effects)";
				return Mode::kOff;
			}
			const auto& a = Addr();
			if (!a.HasAll(A::SpawnBullet, A::CSBulletManager)) {
				a_why = "CSBulletManager / SpawnBullet did not verify (see the address table)";
				return Mode::kOff;
			}
			if (!a.Instance(A::CSBulletManager)) {
				a_why = "CSBulletManager not created yet";
				return Mode::kOff;
			}
			if (!params::Available()) {
				a_why = "SoloParamRepository unverified or not created yet";
				return Mode::kOff;
			}
			return Mode::kBullet;
		}

		std::optional<Row> ReadRow(std::uint32_t a_id)
		{
			auto* at = g_bullet.Row(a_id);
			Row   row{};
			if (!at || !SafeReadBytes(at, row.data(), row.size())) {
				return std::nullopt;
			}
			return row;
		}

		// An object-only attack (AtkParam_Pc [Combat] breaker_atk / explosion_atk), patched once. a_heavy: the blasts' one.
		const ObjAttack* PrepareAttack(bool a_heavy)
		{
			const auto&         c = Cfg();
			auto&               o = g_objAtk[a_heavy ? 1 : 0];
			const std::uint32_t id = a_heavy ? c.explosionAtk : c.breakerAtk;
			const int           power = std::clamp(a_heavy ? c.explosionObjAttack : c.breakerObjAttack, 0, 9999);
			if (o.ok && o.id == id && o.power == power) {
				return &o;
			}
			const std::string before = o.why;
			auto fail = [&](std::string a_why) -> const ObjAttack* {
				o.ok = false;
				o.why = std::move(a_why);
				if (o.why != before) {
					Log::Warn("explosion: {} object-only attack unavailable: {}", a_heavy ? "heavy" : "light", o.why);
				}
				return nullptr;
			};
			if (c.breakerAtk == c.explosionAtk) {
				return fail(std::format("breaker_atk and explosion_atk are both {} (two strengths need two rows)", id));
			}
			// Not the combat route's own attack row (the hit bullets patch theirs per hit).
			if (auto* hitRow = g_bullet.Row(c.bulletId); hitRow && SafeRead<std::int32_t>(hitRow + params::bullet::kAtkId).value_or(0) == static_cast<std::int32_t>(id)) {
				return fail(std::format("AtkParam_Pc {} is the combat route's", id));
			}
			auto*  at = g_atk.Row(id);
			AtkRow row{};
			if (!at || !SafeReadBytes(at, row.data(), row.size())) {
				return fail(std::format("AtkParam_Pc {} not found ({})", id, g_atk.Describe()));
			}
			explosion::MakeObjectOnlyAttack(row.data(), static_cast<std::uint16_t>(power), c.breakerHitsCharacters);
			if (!explosion::ObjectOnly(row.data())) {
				return fail("the patched row still carries damage (bug)");
			}
			if (!g_patch.Write(at, 0, row.data(), row.size())) {
				return fail(std::format("could not write AtkParam_Pc {}", id));
			}
			o.ok = true;
			o.id = id;
			o.power = power;
			o.why.clear();
			Log::Info("explosion: AtkParam_Pc {} = the {} object-only attack (no damage, no flinch, atk_obj {}, {})", id, a_heavy ? "heavy" : "light", power,
				c.breakerHitsCharacters ? "characters still targeted (test)" : "no character targets");
			return &o;
		}

		// Copies kind a_k's template into its carrier as an effect-only bullet (once; again if the ini/console changed it).
		bool Prepare(Kind a_k)
		{
			const auto  k = static_cast<std::size_t>(a_k);
			auto&       p = g_prep[k];
			const auto& c = Cfg();
			const bool  breaks = c.breakObjects && explosion::IsBreaker(a_k);
			const bool  heavy = a_k == Kind::kShrapnel;
			const auto& want = g_objAtk[heavy ? 1 : 0];
			if (p.ok && p.templateId == c.explosionTemplate[k] && p.sfx == c.explosionSfx[k] && p.carrier == c.explosionCarrier[k] && p.breaks == breaks &&
				(!breaks || (want.ok && want.id == (heavy ? c.explosionAtk : c.breakerAtk)))) {
				return true;
			}
			const std::string before = p.why;
			p = {};
			p.templateId = c.explosionTemplate[k];
			p.sfx = c.explosionSfx[k];
			p.carrier = c.explosionCarrier[k];
			auto fail = [&](std::string a_why) {
				p.ok = false;
				p.why = std::move(a_why);
				if (p.why != before) {  // once per reason, not per blast
					Log::Warn("explosion: {} look unavailable: {}", explosion::Name(a_k), p.why);
				}
				return false;
			};
			// A carrier must be nobody else's row: not a template, not the combat route's bullet, not part of a chain we spawn.
			for (std::size_t j = 0; j < explosion::kKinds; ++j) {
				if (c.explosionCarrier[j] == c.explosionTemplate[k] || (j != k && c.explosionCarrier[j] == p.carrier)) {
					return fail(std::format("carrier {} is also a template or another kind's carrier ([Combat] explosion_carriers)", c.explosionCarrier[j]));
				}
			}
			if (p.carrier == c.bulletId) {
				return fail(std::format("carrier {} is the combat route's bullet_id", p.carrier));
			}
			const ObjAttack* atk = breaks ? PrepareAttack(heavy) : nullptr;
			if (breaks && !atk) {
				return fail("no object-only attack: " + g_objAtk[heavy ? 1 : 0].why);
			}
			if (explosion::IsBreaker(a_k) && !c.breakObjects) {
				return fail("break_objects = false");
			}
			auto tmpl = ReadRow(p.templateId);
			if (!tmpl) {
				return fail(std::format("template Bullet {} not found ({})", p.templateId, g_bullet.Describe()));
			}
			auto* carrierRow = g_bullet.Row(p.carrier);
			if (!carrierRow) {
				return fail(std::format("carrier Bullet {} not found", p.carrier));
			}
			std::map<std::uint32_t, Row> cache;
			const auto chain = explosion::WalkChain(tmpl->data(), [&](std::uint32_t a_id) -> const std::uint8_t* {
				if (auto it = cache.find(a_id); it != cache.end()) {
					return it->second.data();
				}
				const auto row = ReadRow(a_id);
				return row ? cache.emplace(a_id, *row).first->second.data() : nullptr;
			});
			for (const auto id : chain.ids) {
				if (std::ranges::find(c.explosionCarrier, id) != c.explosionCarrier.end()) {
					return fail(std::format("carrier {} is part of template {}'s chain", id, p.templateId));
				}
			}
			Row carrier = *tmpl;
			if (explosion::IsBreaker(a_k)) {
				// A breaker flies from breaker_lead before the point to as far past it; shrapnel out to the blast's radius (its life is
				// the longest blast's: explosion_radius 4 m, a hand grenade 7 m), and the spawn picks where it starts.
				const float speed = std::max(1.0f, c.breakerSpeed);
				const float reach = a_k == Kind::kShrapnel ? 7.0f : 2.0f * std::max(0.1f, c.breakerLead);
				explosion::MakeBreaker(carrier.data(), static_cast<std::int32_t>(atk->id), c.breakerRadius, std::clamp(reach / speed, 0.03f, 1.0f), speed);
			} else {
				// The children (sparks, ground flames) stay only if none of them can hurt anyone either.
				// A muzzle flash keeps none (the Fire Pot's 18 sparks and ground flames at every shot would be a fire show).
				p.chainKept = chain.harmless && chain.complete && a_k != Kind::kMuzzle;
				explosion::MakeCarrier(carrier.data(), p.sfx, p.chainKept);
				if (a_k == Kind::kFlame) {
					explosion::MakeSingleShot(carrier.data(), c.flameSpeed);
				}
				if (a_k == Kind::kMuzzle) {
					explosion::MakeFlash(carrier.data(), c.muzzleLife);
				}
				if ((a_k == Kind::kSleep || a_k == Kind::kSmoke) && c.explosionCloudLife > 0.0f) {
					const float life = std::clamp(c.explosionCloudLife, 0.5f, 30.0f);
					std::memcpy(carrier.data() + params::bullet::kLife, &life, 4);
				}
				if (explosion::FlightSfx(carrier.data()) < 0) {
					return fail(std::format("template {} has no flight effect and no sfx is given: nothing to show", p.templateId));
				}
				if (explosion::Harmful(carrier.data())) {
					return fail("carrier still has an attack or SpEffect (bug)");
				}
			}
			p.breaks = atk != nullptr;
			if (explosion::Harmful(carrier.data()) && !(atk && explosion::OnlyAttack(carrier.data(), static_cast<std::int32_t>(atk->id)))) {
				return fail("carrier carries more than the object-only attack (bug)");
			}
			if (!g_patch.Write(carrierRow, 0, carrier.data(), carrier.size())) {
				return fail(std::format("could not write carrier Bullet {}", p.carrier));
			}
			float hitR = 0, hitMax = 0, life = 0;
			std::memcpy(&hitR, carrier.data() + params::bullet::kHitRadius, 4);
			std::memcpy(&hitMax, carrier.data() + params::bullet::kHitRadiusMax, 4);
			std::memcpy(&life, carrier.data() + params::bullet::kLife, 4);
			p.native = explosion::NativeRadius(hitR, hitMax);
			p.life = std::isfinite(life) ? std::clamp(life, 0.0f, 60.0f) : 0.0f;
			p.ok = true;
			Log::Info("explosion: {} = Bullet {} (effect {}, {:.1f} m per burst, {:.2f} s{}) as carrier Bullet {} ({})", explosion::Name(a_k), p.templateId,
				explosion::FlightSfx(carrier.data()), p.native, p.life,
				chain.ids.empty() || explosion::IsBreaker(a_k) ? "" : (p.chainKept ? std::format(", its {} harmless child rows kept", chain.ids.size()) : ", children cut (they could hurt)"),
				p.carrier, atk ? std::format("only the object-only attack {} (atk_obj {})", atk->id, atk->power) : std::string("no attack, no SpEffect"));
			return true;
		}

		// Our bullets in Elden Ring's live list (walked at most once a frame, only when something is about to spawn).
		const Live& CountLive(const Frame& a_f)
		{
			if (g_live.frame == a_f.index) {
				return g_live;
			}
			g_live = {};
			g_live.frame = a_f.index;
			g_spawnedThisFrame = 0;
			auto* m = Addr().Instance(A::CSBulletManager);
			auto* b = m ? SafePtr(m + kListHead) : nullptr;
			g_live.emitters = m ? static_cast<int>(SafeRead<std::uint32_t>(m + 0x1D4).value_or(0)) : 0;
			const auto& c = Cfg();
			for (; b && g_live.listed < kListMax; ++g_live.listed) {
				for (const auto off : kStateIds) {
					const auto id = SafeRead<std::int32_t>(b + off).value_or(0);
					if (id <= 0) {
						continue;
					}
					for (std::size_t k = 0; k < explosion::kKinds; ++k) {
						if (static_cast<std::uint32_t>(id) == c.explosionCarrier[k]) {
							++g_live.byKind[k];
							++g_live.total;
						}
					}
					break;
				}
				b = SafePtr(b + kNextBullet);
			}
			return g_live;
		}

		// How many more of our bullets may be spawned now ([Combat] explosion_max_live).
		int Room(const Frame& a_f)
		{
			const auto& live = CountLive(a_f);
			// And never the last 64 of Elden Ring's 256 emitters or the last 80 of its 256 bullets (the world's own attacks and our
			// hit bullets need them): a Fire Pot burst alone brings 18 sparks (live: 7 bursts = 133 bullets for ~0.5 s).
			return std::max(0, std::min({ Cfg().explosionMaxLive - live.total, 192 - live.emitters, 176 - live.listed }) - g_spawnedThisFrame);
		}

		// a_p dropped onto Elden Ring's ground below/above it (a short ray), if there is ground close by.
		Vec3 OnGround(const Frame& a_f, Vec3 a_p)
		{
			if (!game::CanCastRay()) {
				return a_p;
			}
			Vec3 hit;
			if (game::CastRay(a_p + Vec3{ 0, 2.0f, 0 }, Vec3{ 0, -6.0f, 0 }, hit, a_f.player.chr, Cfg().rayFilter) && Finite(hit)) {
				a_p.y = hit.y + 0.05f;
			}
			return a_p;
		}

		bool SpawnOne(const Frame& a_f, std::uint32_t a_bulletId, const Vec3& a_pos, const Vec3& a_dir)
		{
			const auto& a = Addr();
			auto*       manager = a.Instance(A::CSBulletManager);
			const auto  owner = SafeRead<std::uint64_t>(a_f.player.chr + game::kChrHandle);
			if (!manager || !owner) {
				return false;
			}
			alignas(16) std::uint8_t data[spawn::kSize];
			spawn::Fill(data, *owner, static_cast<std::int32_t>(a_bulletId), spawn::kNoTarget, a_pos, a_dir);
			std::int32_t out = -1, error = 0;
			a.Fn<spawn::SpawnBulletFn>(A::SpawnBullet)(manager, &out, data, &error);
			const auto now = ::GetTickCount64();
			if (out == -1) {
				++g_refused;
				++g_refusedRun;
				g_lastRefusal = now;
				if (g_refusedRun == 1 || now - g_lastRefusalWarn >= 10000) {
					g_lastRefusalWarn = now;
					Log::Warn("explosion: Elden Ring refused SpawnBullet({}) (error {}: 3 = bad bullet id, 4 = construction failed) -- {} in a row, {} in all; {} "
					          "of ours alive, {} listed, emitters {}; clouds wait until a spawn goes through again",
						a_bulletId, error, g_refusedRun, g_refused, g_live.total, g_live.listed, g_live.emitters);
				}
				return false;
			}
			if (g_refusedRun) {
				Log::Info("explosion: Elden Ring takes our spawns again (after {} refused)", g_refusedRun);
				g_refusedRun = 0;
			}
			++g_spawnedThisFrame;
			g_lastSpawn = now;
			return true;
		}

		void Restore(std::string_view a_why)
		{
			if (g_patch.Empty()) {
				return;
			}
			g_patch.Restore();
			for (auto& p : g_prep) {
				p = {};
			}
			g_objAtk = {};
			g_gate.Clear();
			Log::Info("explosion: carrier Bullet rows and the object-only attack restored ({})", a_why);
		}

		bool Ready(const Frame& a_f)
		{
			std::string why;
			if (Effective(why) == Mode::kOff) {
				++g_off;
				if (why != g_lastOffReason) {
					g_lastOffReason = why;
					Log::Info("explosion visuals off: {}", why);
				}
				return false;
			}
			g_lastOffReason.clear();
			return static_cast<bool>(a_f.player);
		}
	}

	void Show(const Frame& a_f, Kind a_kind, const Vec3& a_centre, float a_radius, bool a_onGround, std::string_view a_what)
	{
		if (!Ready(a_f) || !Finite(a_centre)) {
			return;
		}
		const auto& c = Cfg();
		const auto  now = ::GetTickCount64();
		const bool  cloud = explosion::IsCloud(a_kind);
		Vec3        centre = a_onGround || cloud ? (c.explosionGround ? OnGround(a_f, a_centre) : a_centre) : a_centre;
		if (!Prepare(a_kind)) {
			return;
		}
		const auto& p = g_prep[static_cast<std::size_t>(a_kind)];
		if (cloud && g_refusedRun && now - g_lastRefusal < kRefusalBackoffMs) {
			++g_backedOff;  // Elden Ring is refusing spawns: a cloud's refresh only adds to the pressure (the first burst of a blast still tries)
			return;
		}
		if (cloud) {
			// A cloud is refreshed when its last bursts are gone (their bullets live explosion_cloud_life), not on every MGSV report.
			const double repeat = std::max<double>({ 0.2, c.explosionCloudRepeat, p.life - 0.4 }) * 1000.0;
			if (!g_gate.Allow(a_kind, centre, static_cast<double>(now), repeat, std::max(2.5f, a_radius * 0.5f))) {
				++g_gated;  // MGSV's next puff of a cloud we refreshed a moment ago
				return;
			}
		}
		// Bursts face away from the Tarnished (as a pot thrown from there would), level.
		Vec3 dir = centre - game::Position(a_f.player);
		dir.y = 0;
		if (LengthXZ(dir) < 0.1f) {
			dir = Rotate(game::Orientation(a_f.player), { 0, 0, 1 });
			dir.y = 0;
		}
		dir = LengthXZ(dir) > 1e-3f ? Normalize(dir) : Vec3{ 0, 0, 1 };
		g_limiter.Configure(std::max(1.0f, c.explosionPerSecond), std::max(4.0f, c.explosionPerSecond / 2.0f));
		const float phase = static_cast<float>(g_phase++ % 7) * 0.45f;
		const auto  offsets = explosion::Layout(std::max(0.0f, a_radius), p.native, std::clamp(c.explosionMaxBursts, 1, 16), phase);
		int         spawned = 0, dropped = 0, capped = 0;
		for (const auto& o : offsets) {
			if (Room(a_f) <= 0) {
				++capped;
				continue;
			}
			if (!g_limiter.Allow(static_cast<double>(now))) {
				++dropped;
				continue;
			}
			Vec3 at = centre + o;
			if (Length(o) > 0.01f && c.explosionGround) {
				at = OnGround(a_f, at);  // the ring follows the terrain around the centre
			}
			if (SpawnOne(a_f, p.carrier, at, dir)) {
				++spawned;
			}
		}
		g_capped += static_cast<std::uint64_t>(capped);
		if (spawned > 0) {
			g_maxLife = std::max(g_maxLife, p.life);
			g_bursts += static_cast<std::uint64_t>(spawned);
		}
		++g_blasts;
		++g_byKind[static_cast<std::size_t>(a_kind)];
		BoomLog("explosion: {} -> {} r {:.1f} m at ({:.2f}, {:.2f}, {:.2f}): {} burst(s) of Bullet {}{}{}{}", a_what, explosion::Name(a_kind), a_radius, centre.x,
			centre.y, centre.z, spawned, p.templateId, p.breaks ? " (breaks objects)" : "", dropped ? std::format(", {} over the rate limit", dropped) : "",
			capped ? std::format(", {} over the live cap ({} of ours alive)", capped, g_live.total) : "");
	}

	void Flame(const Frame& a_f, const Vec3& a_from, const Vec3& a_dir)
	{
		const auto& c = Cfg();
		if (!c.flameFx || !Finite(a_from) || !Finite(a_dir) || Length(a_dir) < 0.5f || !Ready(a_f)) {
			return;
		}
		g_flameLimiter.Configure(std::max(0.5f, c.flamePerSecond), std::max(1.0f, c.flamePerSecond / 2.0f));
		if (!g_flameLimiter.Allow(static_cast<double>(::GetTickCount64())) || !Prepare(Kind::kFlame) || Room(a_f) <= 0) {
			return;
		}
		if (SpawnOne(a_f, g_prep[static_cast<std::size_t>(Kind::kFlame)].carrier, a_from, Normalize(a_dir))) {
			++g_flames;
			g_maxLife = std::max(g_maxLife, g_prep[static_cast<std::size_t>(Kind::kFlame)].life);
			if (g_flames == 1) {
				Log::Info("explosion: D-Walker's flamethrower drawn with Elden Ring flames (Bullet {} as carrier {})", c.explosionTemplate[4], c.explosionCarrier[4]);
			}
		}
	}

	void Muzzle(const Frame& a_f, const Vec3& a_muzzle, const Vec3& a_dir, bool a_tranq)
	{
		const auto& c = Cfg();
		if (!c.muzzleFlash || (a_tranq && !c.muzzleTranq) || !Finite(a_muzzle) || !Finite(a_dir) || Length(a_dir) < 0.5f || !Ready(a_f)) {
			return;
		}
		g_muzzleLimiter.Configure(std::max(0.5f, c.muzzlePerSecond), std::max(1.0f, c.muzzlePerSecond / 4.0f));
		if (!g_muzzleLimiter.Allow(static_cast<double>(::GetTickCount64()))) {
			++g_muzzleDropped;  // automatic fire: one flash every few rounds still reads as a flashing muzzle
			return;
		}
		const auto k = static_cast<std::size_t>(Kind::kMuzzle);
		if (g_muzzleLife != c.muzzleLife) {
			g_prep[k].ok = false;  // muzzle_life changed (console / ini): the carrier is rewritten
			g_muzzleLife = c.muzzleLife;
		}
		if (!Prepare(Kind::kMuzzle) || Room(a_f) <= 0) {
			return;
		}
		const Vec3 dir = Normalize(a_dir);
		const Vec3 at = a_muzzle + dir * std::clamp(c.muzzleAhead, 0.0f, 1.0f);
		// Never in the camera's face: MGSV's shot origin is UNVERIFIED as the barrel's end (it might be the aim point near the
		// camera in first person / over the shoulder).
		if (const auto* cam = game::PersCam1()) {
			const auto* t = reinterpret_cast<const float*>(cam + game::kCamMatrix + 0x30);
			if (Length(at - Vec3{ t[0], t[1], t[2] }) < 0.6f) {
				++g_muzzleDropped;
				return;
			}
		}
		if (SpawnOne(a_f, g_prep[k].carrier, at, dir)) {
			++g_muzzles;
			g_maxLife = std::max(g_maxLife, g_prep[k].life);
			if (g_muzzles == 1) {
				Log::Info("explosion: muzzle flashes drawn with Bullet {} cut after {:.2f} s (carrier {})", c.explosionTemplate[k], g_prep[k].life, c.explosionCarrier[k]);
			}
		}
	}

	void Break(const Frame& a_f, const Vec3& a_at, const Vec3& a_dir, std::string_view a_what)
	{
		const auto& c = Cfg();
		if (!c.breakObjects || !Finite(a_at) || !Ready(a_f)) {
			return;
		}
		g_breakLimiter.Configure(std::max(1.0f, c.breakerPerSecond), std::max(2.0f, c.breakerPerSecond / 2.0f));
		if (!g_breakLimiter.Allow(static_cast<double>(::GetTickCount64())) || !Prepare(Kind::kBreaker) || Room(a_f) <= 0) {
			return;
		}
		// It flies through the point along a_dir from breaker_lead before it (Elden Ring tests a bullet against its assets along its
		// flight, as the flying Fire Pot that breaks what it flies into).
		const Vec3 dir = Length(a_dir) > 1e-3f ? Normalize(a_dir) : Vec3{ 0, -1, 0 };
		const Vec3 at = a_at - dir * std::max(0.1f, c.breakerLead);
		if (SpawnOne(a_f, g_prep[static_cast<std::size_t>(Kind::kBreaker)].carrier, at, dir)) {
			++g_breaks;
			if (g_breaks <= 30) {
				Log::Info("explosion: {} -> object-only breaker through ({:.2f}, {:.2f}, {:.2f})", a_what, a_at.x, a_at.y, a_at.z);
			} else {
				Log::Debug("explosion: {} -> object-only breaker through ({:.2f}, {:.2f}, {:.2f})", a_what, a_at.x, a_at.y, a_at.z);
			}
		}
	}

	void Shrapnel(const Frame& a_f, const Vec3& a_centre, float a_radius, std::string_view a_what)
	{
		const auto& c = Cfg();
		if (!c.breakObjects || !c.explosionBreaks || !Finite(a_centre) || !Ready(a_f) || !Prepare(Kind::kShrapnel)) {
			return;
		}
		// Heavy breakers out of the blast, level and a little down, as far as its radius (their life covers 7 m: they are spawned
		// further in for a smaller blast), and one straight down.
		const auto  carrier = g_prep[static_cast<std::size_t>(Kind::kShrapnel)].carrier;
		const int   n = std::clamp(c.shrapnelCount, 0, 16);
		const float reach = 7.0f;
		const float start = std::max(0.0f, reach - std::clamp(a_radius, 0.5f, reach));
		int         spawned = 0;
		for (int i = 0; i <= n && Room(a_f) > 0; ++i) {
			Vec3 dir{ 0, -1, 0 };
			if (i < n) {
				const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(std::max(1, n)) + 0.3f;
				dir = Normalize(Vec3{ std::cos(a), -0.15f, std::sin(a) });
			}
			const Vec3 from = a_centre + Vec3{ 0, 0.4f, 0 } - dir * start;
			spawned += SpawnOne(a_f, carrier, from, dir) ? 1 : 0;
		}
		if (spawned) {
			BoomLog("explosion: {} -> {} heavy breakers (shrapnel) out to {:.1f} m", a_what, spawned, std::min(a_radius, reach));
		}
	}

	void Tick(const Frame& a_f)
	{
		(void)a_f;
		if (g_patch.Empty()) {
			return;
		}
		std::string why;
		const bool  wanted = Puppet::Active() && Effective(why) != Mode::kOff;
		if (wanted) {
			return;
		}
		// Bullets still flying (a 3 s sleep mist) read their row: put it back only once they're long gone.
		const auto quiet = static_cast<ULONGLONG>((g_maxLife + kEffectTail) * 1000.0f);
		if (::GetTickCount64() - g_lastSpawn >= quiet) {
			Restore(Puppet::Active() ? why : "puppet off");
		}
	}

	void OnWorldShift(const Vec3& a_delta) { g_gate.Shift(a_delta); }

	void OnPuppetStop()
	{
		if (!g_patch.Empty()) {
			Log::Info("explosion: puppet stopped; carrier rows go back {:.0f} s after the last burst", g_maxLife + kEffectTail);
		}
	}

	std::string Describe()
	{
		std::string why;
		const auto  mode = Effective(why);
		std::string out = std::format("explosion_fx {} -> {}{}: {} blasts ({} lethal, {} stun, {} sleep, {} smoke), {} bursts, {} flames, {} breaks, {} refused "
		                              "({} in a row now), {} cloud repeats skipped, {} cloud refreshes held back after a refusal, {} over the live cap ({}), {} while "
		                              "off, {} rate-dropped; muzzle flashes {} ({} shown, {} over muzzle_per_second)",
			Configured(), explosion::Name(mode), mode == Mode::kOff ? " (" + why + ")" : "", g_blasts, g_byKind[0], g_byKind[1], g_byKind[2], g_byKind[3], g_bursts,
			g_flames, g_breaks, g_refused, g_refusedRun, g_gated, g_backedOff, g_capped, Cfg().explosionMaxLive, g_off, g_limiter.Dropped(),
			Cfg().muzzleFlash ? "on" : "off", g_muzzles, g_muzzleDropped);
		const auto& c = Cfg();
		for (std::size_t k = 0; k < explosion::kKinds; ++k) {
			const auto& p = g_prep[k];
			out += std::format("\n {:7} Bullet {} sfx {} -> carrier {} at {}: {}", explosion::Name(static_cast<Kind>(k)), c.explosionTemplate[k], c.explosionSfx[k],
				c.explosionCarrier[k], static_cast<void*>(g_bullet.Row(c.explosionCarrier[k])),
				p.ok ? std::format("ready ({:.1f} m/burst, {:.2f} s, chain {}{})", p.native, p.life, p.chainKept ? "kept" : "cut/none", p.breaks ? ", breaks objects" : "")
					 : (p.why.empty() ? "not prepared yet" : p.why));
		}
		for (int i = 0; i < 2; ++i) {
			const auto& o = g_objAtk[i];
			out += std::format("\n {} object-only attack AtkParam_Pc {} (atk_obj {}): {}", i ? "heavy" : "light", i ? c.explosionAtk : c.breakerAtk,
				i ? c.explosionObjAttack : c.breakerObjAttack, o.ok ? "patched" : (o.why.empty() ? "not prepared yet" : o.why));
		}
		out += g_patch.Empty() ? "\n carrier rows: original" : "\n carrier rows: patched";
		return out;
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		const std::string sub = a_w.size() > 1 ? a_w[1] : "status";
		if (sub == "status") {
			return Describe();
		}
		if (sub == "mode" && a_w.size() > 2) {
			if (a_w[2] != "ini" && !explosion::ParseMode(a_w[2])) {
				return "boom mode auto|bullet|sfx|off|ini";
			}
			g_modeOverride = a_w[2] == "ini" ? "" : a_w[2];
			return Describe();
		}
		if (sub == "restore") {
			Restore("console");
			return Describe();
		}
		if (sub == "raw" && a_w.size() > 5 && a_f.player) {
			// Diagnostics: an unmodified Elden Ring bullet row (its own attack and effects!) flown from 1 m in front of the Tarnished's
			// chest at an Elden Ring point -- e.g. the real flying Fire Pot (10030000), to see what Elden Ring itself breaks.
			const auto id = static_cast<std::uint32_t>(std::strtoul(a_w[2].c_str(), nullptr, 10));
			const Vec3 to{ std::strtof(a_w[3].c_str(), nullptr), std::strtof(a_w[4].c_str(), nullptr), std::strtof(a_w[5].c_str(), nullptr) };
			const Vec3 chest = game::Position(a_f.player) + Vec3{ 0, 1.4f, 0 };
			const Vec3 dir = Normalize(to - chest);
			const bool ok = SpawnOne(a_f, id, chest + dir * 1.0f, dir);
			return std::format("raw Bullet {} {} toward ({:.2f}, {:.2f}, {:.2f})", id, ok ? "spawned" : "refused", to.x, to.y, to.z);
		}
		if (sub == "set" && a_w.size() > 3) {
			// boom set breaker_speed|breaker_radius|breaker_lead|breaker_obj|explosion_obj <v> | hits_characters on|off: live (not saved);
			// the breaker look and the attacks are rebuilt on their next use.
			auto&       c = Cfg();
			const float v = std::strtof(a_w[3].c_str(), nullptr);
			if (a_w[2] == "breaker_speed") {
				c.breakerSpeed = v;
			} else if (a_w[2] == "breaker_radius") {
				c.breakerRadius = v;
			} else if (a_w[2] == "breaker_lead") {
				c.breakerLead = v;
			} else if (a_w[2] == "breaker_obj") {
				c.breakerObjAttack = static_cast<int>(v);
			} else if (a_w[2] == "explosion_obj") {
				c.explosionObjAttack = static_cast<int>(v);
			} else if (a_w[2] == "hits_characters") {
				c.breakerHitsCharacters = a_w[3] == "on";
			} else if (a_w[2] == "muzzle_flash") {
				c.muzzleFlash = a_w[3] == "on";  // [Combat] muzzle_flash live
			} else {
				return "boom set breaker_speed|breaker_radius|breaker_lead|breaker_obj|explosion_obj <v> | hits_characters|muzzle_flash on|off";
			}
			g_objAtk = {};
			for (auto& p : g_prep) {
				p.ok = false;
			}
			return Describe();
		}
		if (sub == "look" && a_w.size() > 3) {
			// boom look <kind> <bullet id> [sfx]: try another template live (not saved); its carrier is rewritten on the next use.
			const auto kind = explosion::FromName(a_w[2]);
			if (!kind) {
				return "boom look <lethal|stun|sleep|smoke|flame|breaker> <bullet id> [sfx id]";
			}
			const auto k = static_cast<std::size_t>(*kind);
			Cfg().explosionTemplate[k] = static_cast<std::uint32_t>(std::strtoul(a_w[3].c_str(), nullptr, 10));
			Cfg().explosionSfx[k] = a_w.size() > 4 ? static_cast<std::int32_t>(std::strtol(a_w[4].c_str(), nullptr, 10)) : -1;
			return Describe();
		}
		if (sub == "pool") {
			// CSBulletManager's live list, by BulletParam row id (ours and Elden Ring's), and its counters (+0x1D0..: emitters in its
			// buffer, emitters, bullets in buffer, bullets; the lists hold 64 + 192 emitters and 128 + 128 bullets: er/BulletSpawn.h).
			auto* m = Addr().Instance(A::CSBulletManager);
			if (!m) {
				return "no CSBulletManager";
			}
			LARGE_INTEGER t0, t1, freq;
			::QueryPerformanceCounter(&t0);
			std::map<std::int32_t, int> ids;
			int                         n = 0;
			for (auto* b = SafePtr(m + kListHead); b && n < kListMax; ++n) {
				std::int32_t id = 0;
				for (const auto off : kStateIds) {
					id = SafeRead<std::int32_t>(b + off).value_or(0);
					if (id > 0) {
						break;
					}
				}
				++ids[id];
				b = SafePtr(b + kNextBullet);
			}
			::QueryPerformanceCounter(&t1);
			::QueryPerformanceFrequency(&freq);
			std::string out = std::format("CSBulletManager {}: emitters {} in buffer / {} of 256, bullets {} in buffer / {} of 256; {} bullets in the live list ({:.3f} ms to walk):", static_cast<void*>(m),
				SafeRead<std::uint32_t>(m + 0x1D0).value_or(0), SafeRead<std::uint32_t>(m + 0x1D4).value_or(0), SafeRead<std::uint32_t>(m + 0x1D8).value_or(0),
				SafeRead<std::uint32_t>(m + 0x1DC).value_or(0), n, static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart));
			for (const auto& [id, count] : ids) {
				const auto& c = Cfg();
				const auto  ours = std::ranges::find(c.explosionCarrier, static_cast<std::uint32_t>(id));
				out += std::format("\n  Bullet {} x{}{}", id, count,
					ours != c.explosionCarrier.end() ? std::format(" (ours: {})", explosion::Name(static_cast<Kind>(ours - c.explosionCarrier.begin()))) : "");
			}
			return out;
		}
		const auto kind = explosion::FromName(sub);
		if (!kind) {
			return "boom [status] | boom <lethal|stun|sleep|smoke|flame|breaker|shrapnel|muzzle> [radius m] | boom pool | boom set ... | boom look <kind> <bullet> "
			       "[sfx] | boom mode auto|bullet|sfx|off|ini | boom restore";
		}
		if (!a_f.player) {
			return "no player";
		}
		const float radius = a_w.size() > 2 ? std::strtof(a_w[2].c_str(), nullptr) : (*kind == Kind::kLethal ? Cfg().explosionRadius : 4.0f);
		Vec3        fwd = Rotate(game::Orientation(a_f.player), { 0, 0, 1 });
		fwd.y = 0;
		fwd = LengthXZ(fwd) > 1e-3f ? Normalize(fwd) : Vec3{ 0, 0, 1 };
		const Vec3 feet = game::Position(a_f.player);
		if (*kind == Kind::kFlame) {
			g_flameLimiter = combat::RateLimiter{ 6.0f, 3.0f };
			Flame(a_f, feet + Vec3{ 0, 1.5f, 0 } + fwd * 0.5f, fwd);
			return Describe();
		}
		if (*kind == Kind::kMuzzle) {
			// A flash 1.5 m in front of the Tarnished's chest, along his facing (whatever muzzle_flash says).
			const bool was = Cfg().muzzleFlash;
			Cfg().muzzleFlash = true;
			g_muzzleLimiter = combat::RateLimiter{ 10.0f, 3.0f };
			Muzzle(a_f, feet + Vec3{ 0, 1.4f, 0 } + fwd * 1.5f, fwd, false);
			Cfg().muzzleFlash = was;
			return Describe();
		}
		if (*kind == Kind::kBreaker) {
			// At waist height, a_w[2] m (default 1.2) in front of the Tarnished.
			const float ahead = a_w.size() > 2 ? std::clamp(radius, 0.3f, 10.0f) : 1.2f;
			Break(a_f, feet + fwd * ahead + Vec3{ 0, 0.6f, 0 }, fwd, "console break");
			return Describe();
		}
		if (*kind == Kind::kShrapnel) {
			Shrapnel(a_f, feet + fwd * 5.0f, std::clamp(std::isfinite(radius) ? radius : 4.0f, 0.5f, 7.0f), "console shrapnel");
			return Describe();
		}
		const Vec3 at = feet + fwd * 5.0f + Vec3{ 0, 0.3f, 0 };
		g_gate.Clear();  // a console cloud always shows
		Show(a_f, *kind, at, std::clamp(std::isfinite(radius) ? radius : 4.0f, 0.5f, 30.0f), true, "console boom");
		return Describe();
	}
}
