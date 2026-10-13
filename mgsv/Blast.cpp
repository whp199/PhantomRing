#include "Blast.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "Logic.h"
#include "Placed.h"
#include "Throw.h"
#include "Verify.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>

namespace phantom::guest::blast
{
	namespace
	{
		using AreaFn = std::uintptr_t (*)(void* a_this, const std::uint8_t* a_desc);
		AreaFn g_original{ nullptr };

		std::atomic<bool>          g_installed{ false };
		std::atomic<bool>          g_ready{ false };
		std::atomic<std::uint64_t> g_frame{ 0 };
		std::mutex                 g_rootLock;
		logic::Vec3                g_root;

		std::atomic<std::uint64_t>                g_calls{ 0 }, g_sent{ 0 }, g_far{ 0 }, g_owned{ 0 }, g_orphans{ 0 }, g_mgsvMines{ 0 }, g_skipped{ 0 };
		std::array<std::atomic<std::uint64_t>, 6> g_byKind{};
		std::atomic<std::uint32_t>                g_lastAttack{ 0 };
		std::atomic<std::uint32_t>                g_lastOther{ 0 };  // last round/ignored attack id (which explosions stayed out)

		// MGSV's blasts happen where Snake is; one this far from him belongs to MGSV's own world (a scripted
		// explosion, a vehicle) and has no place in Elden Ring's.
		constexpr float kMaxDistance = 150.0f;

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		// Whose blast it can be, by attack id: a placed explosive's (C4 197-199, claymore 202-204/451, anti-tank mine 205-207,
		// sleep-gas mine 208-210, EMN mine 211), a gas/smoke cloud (284-287: a grenade's or a mine's), else a thrown one's.
		enum class Source
		{
			kThrown,
			kPlaced,
			kEither,
		};
		Source SourceOf(std::uint32_t a_attack)
		{
			if ((a_attack >= 197 && a_attack <= 199) || (a_attack >= 202 && a_attack <= 211) || a_attack == 451) {
				return Source::kPlaced;
			}
			return a_attack >= 284 && a_attack <= 287 ? Source::kEither : Source::kThrown;
		}

		// A mine's blast (claymore 202-204/451, anti-tank 205-207, sleep-gas 208-210, EMN 211): set off by an Elden Ring enemy
		// only through kHEvDetonate. C4 (197-199) is the player's remote detonation.
		bool MineAttack(std::uint32_t a_attack) { return (a_attack >= 202 && a_attack <= 211) || a_attack == 451; }

		// The kGEvThrow / kGEvPlaced id this blast belongs to (kGEvExplosion pad[1]), 0 = none we know; a_what = which;
		// a_mgsvWorld = a placed mine MGSV set off itself (not through kHEvDetonate): it stays out of Elden Ring; a_ours = Snake's
		// throw or a mine set off for Elden Ring (kHEvDetonate): Elden Ring's blast, MGSV's own may be skipped (MgsvBlasts).
		std::uint32_t OwnerId(std::uint32_t a_attack, logic::BlastKind a_kind, const logic::Vec3& a_at, const char*& a_what, bool& a_mgsvWorld, bool& a_ours)
		{
			const auto source = SourceOf(a_attack);
			a_mgsvWorld = false;
			a_ours = false;
			if (source != Source::kPlaced) {
				if (const auto id = throwing::MatchBlast(a_at, a_kind)) {
					a_what = "throw";
					a_ours = true;
					return id;
				}
			}
			if (source != Source::kThrown) {
				bool setOff = false;
				if (const auto id = placed::MatchBlast(a_at, a_kind, &setOff)) {
					a_what = "placed";
					// MGSV's mines answer MGSV's characters: D-Walker walking into a claymore (live: one went off the moment it
					// was placed next to the walker), a grenade's blast next to a mine. In Elden Ring only its enemies set them off.
					a_mgsvWorld = !setOff && (MineAttack(a_attack) || (a_attack >= 284 && a_attack <= 287));
					a_ours = setOff;
					return id;
				}
			}
			a_what = "nothing of ours";
			// An unknown mine's blast (set off before it was ever seen placed) is MGSV's world too.
			a_mgsvWorld = MineAttack(a_attack);
			return 0;
		}

		std::atomic<std::uint32_t> g_nextId{ 1 };

		// True: MGSV's own blast is skipped (the caller doesn't run the original). Only for blasts that went to Elden Ring as
		// ours (a throw, a mine set off through kHEvDetonate) with [Combat] MgsvBlasts off: review, a mine set off for an Elden
		// Ring enemy blasted in MGSV 2-4 m from Snake and D-Walker (every mine lies within the treadmill's disc around him).
		bool Inspect(const std::uint8_t* a_desc)
		{
			const auto d = reinterpret_cast<std::uintptr_t>(a_desc);
			const auto centreAt = Get<std::uintptr_t>(d);
			const auto radius = Get<float>(d + 0x30);
			const auto attack = Get<std::uint16_t>(d + 0x3C);
			const auto cls = Get<std::uint8_t>(d + 0x3E);
			const auto owner = Get<std::uint8_t>(d + 0x3F);
			const auto centre = centreAt ? SafeRead<std::array<float, 3>>(reinterpret_cast<const void*>(*centreAt)) : std::nullopt;
			if (!centre || !radius || !attack) {
				return false;
			}
			const logic::Vec3 at{ (*centre)[0], (*centre)[1], (*centre)[2] };
			if (!at.Finite() || !std::isfinite(*radius) || *radius < 0.0f || *radius > 200.0f) {
				return false;
			}
			const auto kind = logic::KindOfAttack(*attack);
			g_byKind[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
			g_lastAttack.store(*attack, std::memory_order_relaxed);
			if (kind == logic::BlastKind::kRound || kind == logic::BlastKind::kIgnore) {
				g_lastOther.store(*attack, std::memory_order_relaxed);
			}
			float dist = 0.0f;
			{
				std::scoped_lock lock(g_rootLock);
				dist = (at - g_root).Length();
			}
			const bool ready = g_ready.load(std::memory_order_relaxed);
			bool send = ready && kind != logic::BlastKind::kRound && kind != logic::BlastKind::kIgnore && dist <= kMaxDistance;
			// The throw/placed explosive it came from: the host blasts where ITS copy of that one is, not at MGSV's centre.
			const char*         from = "";
			bool                mgsvWorld = false, ours = false;
			const std::uint32_t fromId = send ? OwnerId(*attack, kind, at, from, mgsvWorld, ours) : 0;
			// Also the blast of a round Snake's gun fired (rockets, grenade rounds): Elden Ring blasts it where it hits in its own
			// world (kGEvFire); MGSV's own goes off on its apron, live 4.7 m from Snake when D-Walker stood in the line of fire --
			// it destroyed the walker (session 5: `buddy: D-Walker gone` 24 s after a CGM 25 shot).
			const bool          skip = (send && !mgsvWorld && fromId && ours && !Cfg().mgsvBlasts) ||
			                  (ready && kind == logic::BlastKind::kRound && dist <= kMaxDistance && !Cfg().mgsvBlasts);
			if (send && mgsvWorld) {
				send = false;
				g_mgsvMines.fetch_add(1, std::memory_order_relaxed);
			} else if (send) {
				(fromId ? g_owned : g_orphans).fetch_add(1, std::memory_order_relaxed);
			}
			// Every first blast of an attack id, every round/ignored one, then each id at most every 5 s (a gas cloud
			// puffs twice a second for a minute).
			static std::array<std::atomic<std::uint64_t>, 512> lastLog{};
			const auto now = ::GetTickCount64();
			auto&      slot = lastLog[*attack & 511];
			const bool other = kind == logic::BlastKind::kRound || kind == logic::BlastKind::kIgnore;
			const bool due = now - slot.load(std::memory_order_relaxed) > 5000;
			if (due) {
				slot.store(now, std::memory_order_relaxed);
			}
			if (other || mgsvWorld || due) {
				Log::Info("blast: attack {} ({}) r {:.2f} m class {:#x} owner {} at {:.2f},{:.2f},{:.2f} ({:.1f} m from Snake){}", *attack,
					logic::BlastKindName(kind), *radius, cls.value_or(0), owner.value_or(0), at.x, at.y, at.z, dist,
					send        ? std::format(" -> kGEvExplosion (of {} #{}){}", from, fromId, skip ? "; MGSV's own blast skipped (Elden Ring's)" : "")
					: mgsvWorld ? std::format(" (a mine MGSV set off itself ({} #{}), not an Elden Ring enemy: kept out)", from, fromId)
					: !ready    ? std::string(" (not ready)")
					: dist > kMaxDistance ? std::string(" (too far: MGSV's world)")
					: skip ? std::string(" -- MGSV's own blast skipped (Elden Ring blasts the round where it hits)") : std::string());
			}
			if (!send) {
				if (dist > kMaxDistance) {
					g_far.fetch_add(1, std::memory_order_relaxed);
				}
				if (skip) {
					g_skipped.fetch_add(1, std::memory_order_relaxed);
				}
				return skip;
			}
			proto::GuestEvent e{};
			e.type = proto::kGEvExplosion;
			e.a = ProtoKind(kind);
			e.b = static_cast<std::uint32_t>(std::lround(*radius * 1000.0f));
			e.frame = static_cast<std::uint32_t>(g_frame.load(std::memory_order_relaxed));
			e.v0[0] = at.x;
			e.v0[1] = at.y;
			e.v0[2] = at.z;
			e.v1[1] = 1.0f;
			// Not in the protocol's fields: MGSV's attack id, the BLA row's class byte and the owner index (logged by the host).
			e.pad[0] = std::uint32_t(*attack) | (std::uint32_t(cls.value_or(0)) << 16) | (std::uint32_t(owner.value_or(0)) << 24);
			e.pad[1] = fromId;  // v5: the kGEvThrow / kGEvPlaced id (0 = none: MGSV's centre is all there is)
			if (link::PushEvent(e)) {
				g_sent.fetch_add(1, std::memory_order_relaxed);
			}
			if (skip) {
				g_skipped.fetch_add(1, std::memory_order_relaxed);
			}
			return skip;
		}

		std::uintptr_t Detour(void* a_this, const std::uint8_t* a_desc)
		{
			if (reinterpret_cast<std::uintptr_t>(a_this) == addr::kAttackSystem && a_desc) {
				g_calls.fetch_add(1, std::memory_order_relaxed);
				// Every caller ignores the result (the thrown-weapon system's two calls jump on, the Placed System's returns; Ghidra:
				// 0x1415D9806, 0x1415DA4A2, 0x140FC2BDC), so a skipped blast returns 0.
				if (Inspect(a_desc)) {
					return 0;
				}
			}
			return g_original(a_this, a_desc);
		}
	}

	std::uint32_t NextId() { return g_nextId.fetch_add(1, std::memory_order_relaxed); }

	std::uint32_t ProtoKind(logic::BlastKind a_kind)
	{
		switch (a_kind) {
		case logic::BlastKind::kTranq:
			return proto::kAmmoTranq;
		case logic::BlastKind::kStun:
			return proto::kAmmoStun;
		case logic::BlastKind::kNone:
			return proto::kAmmoSmoke;
		default:
			return proto::kAmmoLethal;
		}
	}

	bool Install()
	{
		if (!Cfg().blasts) {
			Log::Info("blast: [Combat] Blasts = false -- thrown explosives send nothing");
			return false;
		}
		if (!Matches(addr::kAttackArea, "attack area vf0") || !Matches(addr::kAttackAreaFragCall, "attack area: frag grenade call") ||
			!Matches(addr::kAttackAreaGasCall, "attack area: gas cloud call")) {
			Log::Warn("blast: attack-area system differs -- thrown explosives send nothing");
			return false;
		}
		if (!Hooks::Create("attack area", reinterpret_cast<void*>(addr::kAttackArea.va), &Detour, &g_original)) {
			return false;
		}
		g_installed = true;
		Log::Info("blast: thrown/placed explosives -> kGEvExplosion (attack-area system vf0 {:#x})", addr::kAttackArea.va);
		return true;
	}

	void Observe(bool a_ready, std::uint64_t a_frame, const float a_root[3])
	{
		g_ready.store(a_ready, std::memory_order_relaxed);
		g_frame.store(a_frame, std::memory_order_relaxed);
		std::scoped_lock lock(g_rootLock);
		g_root = { a_root[0], a_root[1], a_root[2] };
	}

	std::string Describe()
	{
		if (!g_installed) {
			return "blast: off";
		}
		// The system's vtable once the game built it: a different one means the object isn't the one analysed (logged here,
		// the detour filters on the object's address anyway).
		const auto vt = Get<std::uintptr_t>(addr::kAttackSystem).value_or(0);
		return std::format("blast: {} calls, {} sent ({} with a throw/placed id, {} without; {} of ours not blasted in MGSV (MgsvBlasts {}); {} mines MGSV set off itself kept out; lethal {} sleep {} stun {} smoke {}; rounds {} ignored {}, far {}), last attack {}, last kept out {}{}",
			g_calls.load(), g_sent.load(), g_owned.load(), g_orphans.load(), g_skipped.load(), Cfg().mgsvBlasts, g_mgsvMines.load(), g_byKind[0].load(), g_byKind[1].load(), g_byKind[2].load(), g_byKind[3].load(), g_byKind[4].load(), g_byKind[5].load(),
			g_far.load(), g_lastAttack.load(), g_lastOther.load(), vt == addr::kAttackSystemVtable ? "" : std::format(" (system vtable {:#x}!)", vt));
	}
}
