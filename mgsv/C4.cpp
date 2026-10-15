#include "C4.h"

#include "Addresses.h"
#include "C4Logic.h"
#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Placed.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>

namespace phantom::guest::c4
{
	namespace
	{
		// The support slot in hand (vars.currentInventorySlot; Throw.cpp reads it the same way).
		constexpr std::uint32_t kSupportSlot = 3;

		std::mutex g_lock;  // never held across a call into MGSV or Placed.cpp's SetOff detour (it calls NoteMgsvSetOff)
		Detonator  g_det;
		bool       g_on{ false };
		std::uint16_t g_inHand{ 0 };  // the C4 in hand, last Update (0: none)
		std::uint32_t g_guestSetOffs{ 0 }, g_refusedRequests{ 0 }, g_mgsvDetonator{ 0 }, g_mgsvOther{ 0 }, g_unanswered{ 0 };
		std::string   g_lastRefusal;
		// The guest's last grace-time set-off: MGSV's own detonator may still come for that press (an animation first?) -- it
		// must not set off a second C4 then.
		std::uint64_t g_fallbackMs{ 0 };
		std::uint32_t g_lateRequests{ 0 };
		std::uint32_t g_fallbackPress{ 0 };  // Detonator::Presses() then: a newer press's request is that one's own
		constexpr std::uint64_t kLateMs = 2000;

		// PlayerDetonate(ctx, status, player): returned as is (one caller tail-jumps to it, so its rax is that caller's result).
		using PlayerDetonateFn = std::uint64_t (*)(void* a_ctx, std::uint8_t* a_status, std::uint32_t a_player);
		PlayerDetonateFn g_playerDetonate{ nullptr };

		std::uint64_t PlayerDetonateDetour(void* a_ctx, std::uint8_t* a_status, std::uint32_t a_player)
		{
			const auto at = reinterpret_cast<std::uintptr_t>(a_status);
			const auto flagsA = SafeRead<std::uint32_t>(reinterpret_cast<const void*>(at + addr::kStatusFlagsA)).value_or(0);
			const auto flagsB = SafeRead<std::uint32_t>(reinterpret_cast<const void*>(at + addr::kStatusFlagsB)).value_or(0);
			const auto equip = SafeRead<std::uint32_t>(reinterpret_cast<const void*>(at + addr::kStatusEquip)).value_or(0);
			const bool request = (flagsB & (1u << 19)) != 0;
			if (request && IsC4(static_cast<std::uint16_t>(equip))) {
				std::uint64_t since = 0;
				{
					std::scoped_lock lock(g_lock);
					const auto now = ::GetTickCount64();
					if (g_fallbackMs && now - g_fallbackMs <= kLateMs && g_det.Presses() == g_fallbackPress) {
						since = std::max<std::uint64_t>(now - g_fallbackMs, 1);
						g_fallbackMs = 0;
						++g_lateRequests;
					}
				}
				if (since) {
					// This press was answered already: only drop the request, as MGSV does itself (bit 19); nothing reads the
					// result of this call (its callers ignore rax, the tail-jumping state function is a table entry).
					__atomic_fetch_and(reinterpret_cast<std::uint32_t*>(at + addr::kStatusFlagsB), ~(1u << 19), __ATOMIC_SEQ_CST);
					Log::Info("c4: MGSV's detonator came ~{} ms after the press, the guest had set the C4 off at {} ms -- no second C4 "
					          "(a [Combat] C4GraceMs above that lets MGSV's own answer)", since + static_cast<std::uint64_t>(Cfg().c4GraceMs), Cfg().c4GraceMs);
					return 0;
				}
			}
			const auto before = placed::MgsvC4SetOffs();
			const auto result = g_playerDetonate(a_ctx, a_status, a_player);
			if (!request || !IsC4(static_cast<std::uint16_t>(equip)) || placed::MgsvC4SetOffs() != before) {
				return result;  // not C4's, or MGSV's own detonator answered it (the SetOff detour noted it)
			}
			// MGSV took the press and set nothing off: why, for the live test (bit 0 of +0x3C0 and bit 25 of +0x3CC are what
			// it needs), and the guest answers it at once.
			auto why = std::format("player {} status +0x3C0 {:#x} (bit 0 {}) +0x3CC {:#x} (bit 25 {}) equip {}", a_player, flagsA, flagsA & 1,
				flagsB, (flagsB >> 25) & 1, equip);
			const auto n = placed::SetOffC4(static_cast<std::uint16_t>(equip), Cfg().c4DetonateAll, "MGSV's detonator refused the press");
			Log::Info("c4: MGSV's detonator refused the press ({}) -> {} set off by the guest", why, n);
			std::scoped_lock lock(g_lock);
			++g_refusedRequests;
			g_lastRefusal = std::move(why);
			if (n) {
				g_guestSetOffs += n;
				g_det.NoteAnswered(::GetTickCount64());
			}
			return result;
		}
	}

	void Init()
	{
		{
			std::scoped_lock lock(g_lock);
			g_det = Detonator(static_cast<std::uint32_t>(Cfg().c4GraceMs));
			g_on = Cfg().c4Detonator;
		}
		if (!g_on) {
			Log::Info("c4: [Combat] C4Detonator = false -- C4 goes off only through MGSV's own detonator, untouched");
			return;
		}
		// The backup (a tap MGSV never answers) is not additive: MGSV's Y is also Fulton, a supply box, D-Walker, none of which
		// makes a detonate request -- off by default, the unanswered taps are only logged (for the live test).
		const auto backup = Cfg().c4Backup ? std::format("a tap it doesn't answer in {} ms is the guest's ([Combat] C4Backup)", Cfg().c4GraceMs)
		                                   : std::format("a tap it doesn't answer in {} ms is only logged ([Combat] C4Backup off)", Cfg().c4GraceMs);
		// The analysed chain: the press, PlayerDetonate, DetonateOwn's SetOff call (its return address tells the detonator apart).
		const bool chain = Matches(addr::kC4DetonateRequest, "C4 detonate request") && Matches(addr::kPlacedDetonateOwnCall, "C4 DetonateOwn call");
		const bool hooked = chain && Matches(addr::kPlayerDetonate, "C4 PlayerDetonate") &&
		                    Hooks::Create("C4 PlayerDetonate", reinterpret_cast<void*>(addr::kPlayerDetonate.va), &PlayerDetonateDetour, &g_playerDetonate);
		Log::Info("c4: detonator = PlayerPad {:#x} with C4 in hand; MGSV's own {}; {}{}", Cfg().c4Buttons,
			hooked ? "watched (a refused press is answered at once)" : "not watched (sites differ)", backup,
			Cfg().c4DetonateAll ? "; every C4 per press" : "; one C4 per press (the oldest)");
	}

	void Update(bool a_ready, std::uint32_t a_buttons, std::uint32_t a_inHand, std::uint16_t a_support, bool a_free)
	{
		if (!g_on) {
			return;
		}
		const std::uint16_t inHand = a_inHand == kSupportSlot && IsC4(a_support) ? a_support : 0;
		// The cheap checks first: HaveLiveC4 takes Placed.cpp's lock.
		const bool armed = a_ready && a_free && inHand && placed::HaveLiveC4();
		Detonator::Action action;
		{
			std::scoped_lock lock(g_lock);
			g_inHand = inHand;
			action = g_det.Step({ .ms = ::GetTickCount64(), .buttons = a_buttons, .mask = Cfg().c4Buttons, .armed = armed });
		}
		if (action != Detonator::Action::kFallback) {
			return;
		}
		if (!Cfg().c4Backup) {
			std::uint32_t k = 0;
			{
				std::scoped_lock lock(g_lock);
				k = ++g_unanswered;
			}
			// Not set off: the tap may have been MGSV's other ACTION (a supply box, D-Walker). Logged for the live test.
			Log::Info("c4: a tap with C4 in hand got no detonate request from MGSV in {} ms (#{}) -- [Combat] C4Backup off, nothing set off",
				Cfg().c4GraceMs, k);
			return;
		}
		const auto n = placed::SetOffC4(inHand, Cfg().c4DetonateAll, "MGSV's detonator didn't answer the press");
		Log::Info("c4: the detonator press got no answer from MGSV in {} ms -> {} set off by the guest", Cfg().c4GraceMs, n);
		std::scoped_lock lock(g_lock);
		g_guestSetOffs += n;
		g_fallbackMs = n ? ::GetTickCount64() : 0;
		g_fallbackPress = g_det.Presses();
	}

	void NoteMgsvSetOff(bool a_detonator, std::uint16_t a_equip)
	{
		// C4DetonateAll: MGSV's own detonator sets off one (the oldest); the rest of the press is the guest's (review: with MGSV
		// answering, the switch did nothing). Placed.cpp released its lock before calling us; ours isn't held here either.
		const auto n = a_detonator && g_on && Cfg().c4DetonateAll ? placed::SetOffC4(a_equip, true, "C4DetonateAll, after MGSV's detonator") : 0;
		std::scoped_lock lock(g_lock);
		++(a_detonator ? g_mgsvDetonator : g_mgsvOther);
		g_guestSetOffs += n;
		// Only the detonator answers a press: a chain or the sensor setting one off next to a press is not that press's answer.
		if (a_detonator) {
			g_det.NoteAnswered(::GetTickCount64());
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		if (!g_on) {
			return "c4: off";
		}
		return std::format("c4: in hand {}, presses {} (answered {}, fallbacks {}{}, holds {}{}), MGSV's detonator {} / other set-offs {}, refused requests {}{}, "
		                   "set off by the guest {}, late MGSV requests dropped {}",
			g_inHand, g_det.Presses(), g_det.Answered(), g_det.Fallbacks(), Cfg().c4Backup ? "" : std::format(" -- backup off: {} only logged", g_unanswered),
			g_det.Holds(), g_det.Pending() ? ", one pending" : "", g_mgsvDetonator, g_mgsvOther, g_refusedRequests,
			g_lastRefusal.empty() ? "" : " (last: " + g_lastRefusal + ")", g_guestSetOffs, g_lateRequests);
	}
}
