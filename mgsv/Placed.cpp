#include "Placed.h"

#include "Addresses.h"
#include "Blast.h"
#include "C4.h"
#include "C4Logic.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <format>
#include <mutex>

namespace phantom::guest::placed
{
	namespace
	{
		using logic::Vec3;
		constexpr std::uint32_t kSlots = addr::kPlacedSlots;

		bool g_varsOk{ false };    // the vars mirror is the one analysed: events
		bool g_systemOk{ false };  // the Placed System is: detonation

		struct Slot
		{
			std::uint32_t        id{ 0 };     // 0 = nothing of ours here
			std::uint16_t        equip{ 0 };
			logic::Explosive     ex;
			Vec3                 pos;
			std::array<float, 4> rot{};
			std::uint32_t        announcedGen{ ~0u };  // the host session it was announced to (~0u: never announced)
			std::uint32_t        announcedStart{ 0 };  // ... and the host's puppet start it was announced to
			bool                 setOff{ false };
			bool                 popped{ false };       // a decoy Elden Ring popped (kHEvDetonate): gone for the host, never announced again
			bool                 preexisting{ false };  // in the vars when the sandbox became ready: MGSV's world (an old save's), never announced
		};
		struct Gone
		{
			std::uint32_t    id{ 0 };
			logic::BlastKind kind{ logic::BlastKind::kIgnore };
			Vec3             pos;
			std::uint64_t    ms{ 0 };
			bool             setOff{ false };  // by us (kHEvDetonate)
		};
		std::mutex                g_lock;
		std::array<Slot, kSlots>  g_slots{};
		std::array<Gone, 16>      g_gone{};
		std::size_t               g_goneNext{ 0 };
		std::uint64_t             g_frame{ 0 };
		std::uint64_t             g_placedEvents{ 0 }, g_goneEvents{ 0 }, g_detonations{ 0 }, g_refused{ 0 }, g_ownExplosions{ 0 }, g_matched{ 0 }, g_pops{ 0 };
		std::string               g_lastRefusal;
		std::atomic<std::uint32_t> g_requested{ 0 };  // console detonation for the next Update
		bool                       g_seenReady{ false };  // the first ready Update is behind us (what lies in the vars then is the save's)
		// The host drops its copies of Snake's mines whenever its puppet stops (er/Mines.cpp OnPuppetStop: a death, a load,
		// a guest heartbeat timeout) and ignores kGEvPlaced while it isn't puppeting; it never asks for them again. So every
		// start of its puppet (HostState kHostPuppeting going up) gets every live one again, mapped through its new anchor.
		logic::RisingEdges         g_puppetStarts;

		// MGSV's own SetOff (Addresses.h kPlacedSetOff), refused for Snake's mines: MGSV's sensors answer MGSV's characters
		// (live: D-Walker, whose follow spot is ahead-left of Snake, set off 3 of 3 claymores 0-1.7 s after they were placed).
		using SetOffFn = bool (*)(std::uint8_t* a_system, std::uint16_t a_setter, std::uint32_t a_object);
		SetOffFn                    g_setOff{ nullptr };
		std::atomic<std::uint64_t>  g_refusedSetOffs{ 0 };
		std::atomic<std::uint32_t>  g_lastRefusedObject{ ~0u };
		std::atomic<std::uint64_t>  g_mgsvC4SetOffs{ 0 };  // Snake's C4 that MGSV's own SetOff set off (C4.cpp)

		bool SetOffDetour(std::uint8_t* a_system, std::uint16_t a_setter, std::uint32_t a_object)
		{
			const auto sys = reinterpret_cast<std::uintptr_t>(a_system);
			const auto first = SafeRead<std::uint32_t>(reinterpret_cast<const void*>(sys + 0x88)).value_or(~0u);
			const auto equips = SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(sys + 0x38)).value_or(0);
			const auto equip = equips ? SafeRead<std::uint16_t>(reinterpret_cast<const void*>(equips + std::uintptr_t(a_object) * 4)) : std::nullopt;
			const auto ex = equip ? logic::ExplosiveOf(*equip) : logic::Explosive{};
			// Snake's own (from the first player index on), a mine (a trigger radius: not C4), on the Placed System we know.
			if (first != ~0u && a_object >= first && ex.use == logic::ExplosiveUse::kPlaced && ex.triggerMm != 0 &&
				SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(sys)).value_or(0) == addr::kPlacedVtable) {
				g_refusedSetOffs.fetch_add(1, std::memory_order_relaxed);
				if (g_lastRefusedObject.exchange(a_object, std::memory_order_relaxed) != a_object) {
					Log::Info("placed: MGSV wanted to set off Snake's mine (object {}, equip {}, by GameObject {:#x}) -- refused: only Elden Ring's "
					          "enemies set his mines off (kHEvDetonate)", a_object, *equip, a_setter);
				}
				return false;
			}
			const bool done = g_setOff(a_system, a_setter, a_object);
			// Snake's own C4 set off by MGSV: its detonator (DetonateOwn; C4.cpp), the blast is the player's -- Elden Ring's, put
			// where the host's copy lies (MGSV's own one skipped with [Combat] MgsvBlasts off, as for a mine of ours).
			if (done && first != ~0u && a_object >= first && equip && c4::IsC4(*equip) && Cfg().c4Detonator) {
				const bool detonator = __builtin_return_address(0) == reinterpret_cast<void*>(addr::kPlacedDetonateOwnReturn);
				std::uint32_t id = 0, other = 0;
				bool          preexisting = false;
				{
					std::scoped_lock lock(g_lock);
					const auto slot = a_object - first;
					if (slot < kSlots && g_slots[slot].id && g_slots[slot].equip == *equip) {
						auto& s = g_slots[slot];
						preexisting = s.preexisting;
						other = s.id;
						// Only one that reached Elden Ring and wasn't on its way yet: an old save's C4 (DetonateOwn takes the
						// oldest, and that one is older than any the player placed) is MGSV's world -- not an answer to the press.
						if (!s.preexisting && !s.setOff) {
							s.setOff = true;
							id = s.id;
						}
					}
				}
				if (!id) {
					// Review: counted as the answer, the player's own C4 in Elden Ring stayed put and MGSV's blast was taken
					// for ours. PlayerDetonate's detour now sees nothing of ours went off and sets the oldest of them off.
					Log::Info("placed #{}: C4 (object {}, equip {}) set off by MGSV{} -- {}: MGSV's world, not the player's answer", other, a_object, *equip,
						detonator ? "'s detonator (DetonateOwn)" : " (not its detonator)",
						preexisting ? "an old save's" : other ? "already on its way" : "never sent to Elden Ring");
					return done;
				}
				g_mgsvC4SetOffs.fetch_add(1, std::memory_order_acq_rel);
				Log::Info("placed #{}: C4 (object {}, equip {}) set off by MGSV{}", id, a_object, *equip,
					detonator ? "'s detonator (DetonateOwn)" : " (not its detonator: the sensor or the update)");
				c4::NoteMgsvSetOff(detonator, *equip);
			}
			return done;
		}

		// A blast belongs to a placed explosive this close: the claymore's sits 2.5 m in front of it (its capsule).
		constexpr float         kMatchRadius = 4.0f;
		// Gone ones are kept for blasts that come later: the sleep-gas mine puffs for ~10 s after it went off.
		constexpr std::uint64_t kKeepMs = 30000;
		// Placed far from Snake = MGSV's world (an old save's mine), not his.
		constexpr float         kMaxDistance = 150.0f;

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		std::uintptr_t VarsBlock() { return Get<std::uintptr_t>(addr::kVarsPtr).value_or(0); }

		// The Placed System (Addresses.h): [[hub+0x98]+0x208] - 0x20, both vtables checked.
		std::uintptr_t System()
		{
			const auto holder = Get<std::uintptr_t>(addr::kHubHolder).value_or(0);
			const auto hub = holder ? Get<std::uintptr_t>(holder + 8).value_or(0) : 0;
			const auto systems = hub ? Get<std::uintptr_t>(hub + addr::kHubSystems).value_or(0) : 0;
			const auto sub = systems ? Get<std::uintptr_t>(systems + addr::kSystemsPlaced).value_or(0) : 0;
			if (sub < 0x20 || Get<std::uintptr_t>(sub).value_or(0) != addr::kPlacedVtable2 ||
				Get<std::uintptr_t>(sub - 0x20).value_or(0) != addr::kPlacedVtable) {
				return 0;
			}
			return sub - 0x20;
		}

		void Push(std::uint32_t a_type, const Slot& a_s, std::uint32_t a_id, bool a_again = false)
		{
			proto::GuestEvent e{};
			e.type = a_type;
			e.b = a_id;
			e.frame = static_cast<std::uint32_t>(g_frame);
			if (a_type == proto::kGEvPlaced) {
				// MGSV's own blast direction for a claymore is its rotation applied to +Z (0x140FC2680); a decoy's facing.
				const Vec3 facing = logic::RotateByQuat(a_s.rot.data(), { 0.0f, 0.0f, 1.0f });
				e.a = a_s.ex.use == logic::ExplosiveUse::kDecoy ? proto::kAmmoDecoy : blast::ProtoKind(a_s.ex.kind);
				e.v0[0] = a_s.pos.x, e.v0[1] = a_s.pos.y, e.v0[2] = a_s.pos.z;
				e.v1[0] = facing.x, e.v1[1] = facing.y, e.v1[2] = facing.z;
				// (v9) Announced before: MGSV's point is where it was placed relative to an earlier anchor, not where it lies in Elden
				// Ring now (review: the host mapped re-announced mines through its new anchor, next to wherever the player was).
				e.pad[0] = a_s.equip | (a_again ? proto::kPlacedAgain : 0u);
				e.pad[1] = a_s.ex.triggerMm;
			}
			link::PushEvent(e);
		}

		void Retire(Slot& a_s, const char* a_why)
		{
			if (!a_s.id) {
				return;
			}
			if (a_s.announcedGen != ~0u && !a_s.popped) {  // a popped decoy's kGEvPlacedGone went out when it popped
				Push(proto::kGEvPlacedGone, a_s, a_s.id);
				++g_goneEvents;
			}
			g_gone[g_goneNext++ % g_gone.size()] = { a_s.id, a_s.ex.kind, a_s.pos, ::GetTickCount64(), a_s.setOff };
			Log::Info("placed #{}: equip {} gone ({}){}", a_s.id, a_s.equip, a_why, a_s.announcedGen != ~0u ? " -> kGEvPlacedGone" : "");
			a_s = {};
		}

		// Sets off our object a_slot (vars index) through MGSV's own path. False = refused (logged by the caller).
		bool SetOff(const Slot& a_s, std::uint32_t a_slot, std::string& a_why)
		{
			const auto sys = g_systemOk ? System() : 0;
			if (!sys) {
				a_why = g_systemOk ? "the Placed System isn't there (vtables)" : "placed system sites differ";
				return false;
			}
			const auto first = Get<std::uint32_t>(sys + 0x88).value_or(~0u);
			const auto count = Get<std::uint32_t>(sys + 0x80).value_or(0);
			const std::uint32_t index = first + a_slot;
			if (first == ~0u || index >= count) {
				a_why = std::format("object index {}+{} past {}", first, a_slot, count);
				return false;
			}
			const auto equips = Get<std::uintptr_t>(sys + 0x38).value_or(0);
			const auto flags = Get<std::uintptr_t>(sys + 0xB0).value_or(0);
			const auto equip = equips ? Get<std::uint16_t>(equips + std::uintptr_t(index) * 4) : std::nullopt;
			const auto live = flags ? Get<std::uint8_t>(flags + std::uintptr_t(index) * 3) : std::nullopt;
			if (!equip || *equip != a_s.equip || !live || !(*live & 1)) {
				a_why = std::format("object {} is equip {} flags {:#x}, not our live {}", index, equip.value_or(0), live.value_or(0), a_s.equip);
				return false;
			}
			// Bit 2 = set off: its views blast it (or puff, or cast the net) on their next update. One atomic byte OR.
			__atomic_fetch_or(reinterpret_cast<std::uint8_t*>(flags + std::uintptr_t(index) * 3), std::uint8_t(4), __ATOMIC_SEQ_CST);
			return true;
		}

		// kHEvDetonate for a decoy: an Elden Ring enemy walked into it (er/Decoys.h) and it popped there, or the host dropped it (set
		// before a load, unknown to this host). The guest is the one that remembers: kGEvPlacedGone now, never announced again (a
		// restarted puppet would otherwise get it back). In MGSV it
		// stays where Snake set it unless [Combat] DecoyPopInMgsv sets its "set off" bit too (UNVERIFIED what MGSV's decoy
		// views do with that bit: deflate, shock, or nothing).
		void PopDecoy(Slot& a_s, std::uint32_t a_slot)
		{
			a_s.popped = true;
			++g_pops;
			if (a_s.announcedGen != ~0u) {
				Push(proto::kGEvPlacedGone, a_s, a_s.id);
				++g_goneEvents;
			}
			std::string why = "off";
			const bool  mgsv = Cfg().decoyPopInMgsv && SetOff(a_s, a_slot, why);
			Log::Info("placed #{}: decoy (equip {}) popped (or dropped) in Elden Ring -> kGEvPlacedGone; in MGSV {}", a_s.id, a_s.equip,
				mgsv ? "its set-off bit is set" : Cfg().decoyPopInMgsv ? "left standing (" + why + ")" : std::string("left standing (DecoyPopInMgsv = false)"));
		}
	}

	void Init()
	{
		if (!Cfg().placed) {
			Log::Info("placed: [Combat] Placed = false -- no kGEvPlaced, kHEvDetonate ignored");
			return;
		}
		g_varsOk = Matches(addr::kVarPlacedWeapons) && Matches(addr::kVarPlacedPos) && Matches(addr::kVarPlacedRot);
		g_systemOk = g_varsOk && Matches(addr::kPlacedRegister, "placed register") && Matches(addr::kPlacedRegisterStore, "placed register store") &&
		             Matches(addr::kPlacedTriggerTest, "placed set-off test") && Matches(addr::kPlacedRemoveFlags, "placed flags");
		// Snake's mines are Elden Ring's to set off: MGSV's own sensors (D-Walker in a claymore's cone) are refused.
		if (g_systemOk && Matches(addr::kPlacedSetOff, "placed set-off") && Matches(addr::kPlacedSensorCall, "placed sensor call") &&
			Hooks::Create("placed set-off", reinterpret_cast<void*>(addr::kPlacedSetOff.va), &SetOffDetour, &g_setOff)) {
			Log::Info("placed: MGSV's own set-off refused for Snake's mines (C4 stays the player's)");
			c4::Init();  // C4's detonator watches that same set-off
		}
		Log::Info("placed: {}{}", g_varsOk ? "placed explosives -> kGEvPlaced/kGEvPlacedGone" : "vars mirror differs -- no placed events",
			g_varsOk ? (g_systemOk ? ", kHEvDetonate sets them off in MGSV" : ", Placed System differs: kHEvDetonate refused") : "");
	}

	void RequestDetonate(std::uint32_t a_id) { g_requested.store(a_id, std::memory_order_release); }

	void Update(bool a_ready, std::uint64_t a_frame, const Vec3& a_root)
	{
		g_frame = a_frame;
		if (!g_varsOk) {
			return;
		}
		if (const auto id = g_requested.exchange(0, std::memory_order_acq_rel)) {
			Detonate(id);
		}
		const auto block = VarsBlock();
		std::array<std::uint16_t, kSlots> weapons{};
		std::array<float, kSlots * 3>     pos{};
		std::array<float, kSlots * 4>     rot{};
		SIZE_T                            got = 0;
		const auto                        rd = [&](std::uint32_t a_off, void* a_out, std::size_t a_n) {
            return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(block + a_off), a_out, a_n, &got) && got == a_n;
		};
		if (!block || !rd(addr::kVarPlacedWeapons.offset, weapons.data(), sizeof(weapons)) || !rd(addr::kVarPlacedPos.offset, pos.data(), sizeof(pos)) ||
			!rd(addr::kVarPlacedRot.offset, rot.data(), sizeof(rot))) {
			return;
		}
		const auto       gen = link::AttachGeneration();
		proto::HostState hs{};
		const bool       puppeting = link::ReadHostState(hs) && (hs.flags & proto::kHostPuppeting);
		const auto       start = g_puppetStarts.Update(puppeting);
		std::scoped_lock lock(g_lock);
		for (std::uint32_t i = 0; i < kSlots; ++i) {
			auto&       s = g_slots[i];
			const auto  w = weapons[i];
			const Vec3  p{ pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2] };
			const auto  ex = logic::ExplosiveOf(w);
			const bool  kind = ex.use == logic::ExplosiveUse::kPlaced || (ex.use == logic::ExplosiveUse::kDecoy && Cfg().decoys);
			const bool  ours = w != 0 && kind && p.Finite() && (p - a_root).Length() <= kMaxDistance;
			if (s.id && (!ours || w != s.equip || (p - s.pos).Length() > 0.5f)) {
				Retire(s, w == 0 ? "removed" : "replaced");
			}
			if (!ours || !a_ready) {
				continue;
			}
			if (!s.id) {
				s.id = blast::NextId();
				s.equip = w;
				s.ex = ex;
				s.pos = p;
				s.preexisting = !g_seenReady;
				std::copy_n(rot.begin() + i * 4, 4, s.rot.begin());
				Log::Info("placed #{}: equip {} ({}) at {:.2f},{:.2f},{:.2f} rot {:.3f},{:.3f},{:.3f},{:.3f} (vars slot {}){}", s.id, w,
					ex.use == logic::ExplosiveUse::kDecoy ? "decoy" : logic::BlastKindName(ex.kind),
					p.x, p.y, p.z, s.rot[0], s.rot[1], s.rot[2], s.rot[3], i,
					s.preexisting ? " -- already in the save when the sandbox loaded: MGSV's world, never sent to Elden Ring" : "");
			}
			if (s.preexisting || s.popped) {
				continue;
			}
			// New, a new host session that never heard of it, or a new start of the host's puppet (it forgot it). Only while
			// the host puppets: it drops a kGEvPlaced otherwise, and the next start sends it then.
			if (puppeting && (s.announcedGen != gen || s.announcedStart != start)) {
				const bool again = s.announcedGen != ~0u;
				if (again) {
					Log::Info("placed #{}: announced again ({})", s.id, s.announcedGen == gen ? "the host's puppet restarted" : "a new host session");
				}
				s.announcedGen = gen;
				s.announcedStart = start;
				Push(proto::kGEvPlaced, s, s.id, again);
				++g_placedEvents;
			}
		}
		g_seenReady = g_seenReady || a_ready;
	}

	void Detonate(std::uint32_t a_id)
	{
		std::scoped_lock lock(g_lock);
		for (std::uint32_t i = 0; i < kSlots; ++i) {
			auto& s = g_slots[i];
			if (s.id != a_id) {
				continue;
			}
			if (s.setOff || s.popped) {
				return;  // already on its way (the host repeats while an enemy stands in the radius)
			}
			if (s.ex.use == logic::ExplosiveUse::kDecoy) {
				PopDecoy(s, i);
				return;
			}
			std::string why;
			if (!SetOff(s, i, why)) {
				++g_refused;
				g_lastRefusal = why;
				Log::Warn("placed #{}: kHEvDetonate refused: {}", a_id, why);
				return;
			}
			s.setOff = true;
			++g_detonations;
			Log::Info("placed #{}: kHEvDetonate -> set off in MGSV (equip {}, vars slot {})", a_id, s.equip, i);
			if (!s.ex.ownBlast) {
				// The EMN mine casts a net, no blast reaches the attack-area system: its explosion is ours.
				proto::GuestEvent e{};
				e.type = proto::kGEvExplosion;
				e.a = blast::ProtoKind(s.ex.kind);
				e.b = static_cast<std::uint32_t>(s.ex.radius * 1000.0f);
				e.frame = static_cast<std::uint32_t>(g_frame);
				e.v0[0] = s.pos.x, e.v0[1] = s.pos.y, e.v0[2] = s.pos.z;
				e.v1[1] = 1.0f;
				e.pad[0] = 211;  // ATK_ElectromagneticNetMine (informational)
				e.pad[1] = a_id;
				link::PushEvent(e);
				++g_ownExplosions;
				Log::Info("placed #{}: no MGSV blast for equip {} -- kGEvExplosion {} r {:.1f} m sent by the guest", a_id, s.equip,
					logic::BlastKindName(s.ex.kind), s.ex.radius);
			}
			return;
		}
		Log::Info("placed: kHEvDetonate for unknown id {} (gone already?)", a_id);
	}

	std::uint32_t SetOffC4(std::uint16_t a_inHand, bool a_all, const char* a_why)
	{
		std::scoped_lock lock(g_lock);
		std::uint32_t n = 0;
		// Refused this call (review): skipped only here, as kHEvDetonate's Detonate does -- a refusal can be momentary (the
		// system pointer, the live bit not set yet), and a C4 marked set off for good could never go off again.
		std::array<bool, kSlots> tried{};
		for (;;) {
			std::array<c4::Candidate, kSlots> all{};
			for (std::uint32_t i = 0; i < kSlots; ++i) {
				const auto& s = g_slots[i];
				// MGSV's world (an old save's) never reached Elden Ring: it isn't the player's to set off there.
				all[i] = { s.preexisting ? 0u : s.id, s.equip, s.setOff || tried[i] };
			}
			const int i = c4::PickOldest(all, a_inHand);
			if (i < 0) {
				return n;
			}
			auto&       s = g_slots[i];
			std::string why;
			if (!SetOff(s, static_cast<std::uint32_t>(i), why)) {
				++g_refused;
				g_lastRefusal = why;
				Log::Warn("placed #{}: C4 detonation ({}) refused: {}", s.id, a_why, why);
				tried[i] = true;  // the next C4 for this press; this one stays live for the next
				continue;
			}
			s.setOff = true;
			++g_detonations;
			++n;
			Log::Info("placed #{}: C4 set off by the guest ({}; equip {}, vars slot {})", s.id, a_why, s.equip, i);
			if (!a_all) {
				return n;
			}
		}
	}

	bool HaveLiveC4()
	{
		std::scoped_lock lock(g_lock);
		for (const auto& s : g_slots) {
			if (s.id && !s.preexisting && !s.setOff && c4::IsC4(s.equip)) {
				return true;
			}
		}
		return false;
	}

	std::uint64_t MgsvC4SetOffs() { return g_mgsvC4SetOffs.load(std::memory_order_acquire); }

	std::uint32_t MatchBlast(const Vec3& a_centre, logic::BlastKind a_kind, bool* a_setOff)
	{
		std::scoped_lock lock(g_lock);
		std::uint32_t best = 0;
		bool          bestSetOff = false;
		float         bestD = kMatchRadius;
		for (const auto& s : g_slots) {
			if (s.id && s.ex.use != logic::ExplosiveUse::kDecoy && s.ex.kind == a_kind && (s.pos - a_centre).Length() <= bestD) {
				bestD = (s.pos - a_centre).Length();
				best = s.id;
				bestSetOff = s.setOff;
			}
		}
		// A live one first: a new claymore where the last one stood is that one, not the old one (live: the second
		// claymore's blast was taken for the first's).
		const auto now = ::GetTickCount64();
		for (const auto& g : g_gone) {
			if (!best && g.id && g.kind == a_kind && now - g.ms <= kKeepMs && (g.pos - a_centre).Length() <= bestD) {
				bestD = (g.pos - a_centre).Length();
				best = g.id;
				bestSetOff = g.setOff;
			}
		}
		g_matched += best != 0;
		if (a_setOff) {
			*a_setOff = bestSetOff;
		}
		return best;
	}

	std::string Describe()
	{
		if (!g_varsOk) {
			return "placed: off";
		}
		std::scoped_lock lock(g_lock);
		std::string live;
		for (std::uint32_t i = 0; i < kSlots; ++i) {
			const auto& s = g_slots[i];
			if (s.id) {
				live += std::format(" #{}={}@{:.1f},{:.1f},{:.1f}{}", s.id, s.equip, s.pos.x, s.pos.y, s.pos.z, s.setOff ? "(set off)" : s.popped ? "(popped)" : "");
			}
		}
		return std::format("placed: {} placed / {} gone events, detonations {} ({} refused{}), decoys popped {}, own explosions {}, blasts matched {}, MGSV set-offs refused {}, "
		                   "system {}; live:{}",
			g_placedEvents, g_goneEvents, g_detonations, g_refused, g_lastRefusal.empty() ? "" : ": " + g_lastRefusal, g_pops, g_ownExplosions, g_matched, g_refusedSetOffs.load(),
			g_systemOk ? (System() ? "ok" : "not found") : "off", live.empty() ? " none" : live) + "\n" + c4::Describe();
	}
}
