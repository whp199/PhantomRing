// D-Dog as a battle buddy in Elden Ring: the game glue (Dog.h; the rules in DogLogic.h; docs/research/ddog.md, docs/GUEST.md 8.4).
#include "Dog.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "BuddyLocation.h"
#include "BuddyLogic.h"
#include "Combat.h"
#include "Config.h"
#include "DogLogic.h"
#include "Follow.h"
#include "Hold.h"
#include "Hook.h"
#include "Hud.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Skeleton.h"
#include "Verify.h"
#include "phantomring_protocol.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace phantom::guest::dog
{
	namespace
	{
		using logic::Vec3;

		using HandlerFn = std::int32_t* (*)(std::uint8_t* a_sys, std::int32_t* a_result, std::uint32_t a_index, std::uint64_t* a_message);
		using SoldierFn = void (*)(std::uint8_t* a_component, std::uint32_t a_local, std::int32_t* a_result, std::uint64_t* a_message);
		using StateFn = void (*)(std::uint8_t* a_owner, std::uint32_t a_index, std::uint32_t a_event, std::uint8_t* a_data);
		using RecordFn = std::uint8_t* (*)(void* a_this, std::uint32_t a_index, std::uint32_t a_zero, std::uint64_t a_key);
		using BoxSendFn = std::int32_t* (*)(void* a_box, std::int32_t* a_result, std::uint16_t a_target, void* a_message);
		using AccessorFn = std::uintptr_t (*)();

		constexpr std::uint64_t kPickWaitMs = 350;  // an order with no fitting pick looks again this long (the host re-picks for it)
		// The host's pick as last seen before the order: the paw the player saw over him with LB held (0.8.0: 2 of 16 orders were refused
		// "out of sight" the instant the order came although the paw had shown). Used when none fits at the order, if seen this recently.
		constexpr std::uint64_t kPickMemoryMs = 1000;
		constexpr std::uint64_t kMsgWhistle = 0xE8345CEE216E, kMsgWait = 0xD429F0083A9E, kMsgBark = 0x0D5CB991EBF6, kMsgAttack = 0xF28E1C4CC23A,
		                        kMsgTalk = 0xEDB2466B86D3;

		int  g_sites{ -1 };  // -1 not checked, 0 differ (the dog layer stays off), 1 ok
		bool g_varsOk{ false };
		bool g_boxOk{ false };
		bool g_recordOk{ false };
		bool g_cacheOk{ false };
		HandlerFn g_handler{ nullptr };
		SoldierFn g_soldier{ nullptr };
		StateFn   g_state{ nullptr };
		bool      g_hooked[3]{};

		// ---- what the detours learn (written on the game thread, read anywhere) ----------------------------------------------
		std::atomic<std::uintptr_t> g_sys{ 0 };      // the dog system (the handler's this)
		std::atomic<std::uint32_t>  g_index{ 0 };    // the dog's index (id 0x2600 | index)
		std::atomic<std::uintptr_t> g_record{ 0 };   // its order record R
		std::atomic<std::uint64_t>  g_messages{ 0 }, g_orderMessages{ 0 };
		std::atomic<int>            g_styleByte{ -1 };  // [sys+0xD4] as read last (Lua thread)

		struct SeenMessage
		{
			std::uint64_t id{ 0 };
			std::uint16_t arg{ 0 };
			std::uint64_t ms{ 0 };
		};
		std::mutex                  g_seenLock;
		std::array<SeenMessage, 12> g_seen{};
		std::size_t                 g_seenAt{ 0 };
		std::vector<std::uint64_t>  g_seenIds;  // every distinct id once (logged once)

		struct Accept
		{
			std::uint16_t soldier{ 0xFFFF };  // GameObject id of the soldier that accepted
			std::uint64_t message{ 0 };
			bool          stun{ false };  // the knife message's +0xA byte: set by the stun knife (mode 2; live 2026-11-04)
			std::uint64_t ms{ 0 };
		};
		std::mutex                 g_acceptLock;
		Accept                     g_lastAccept;
		std::atomic<std::uint64_t> g_accepts{ 0 }, g_refusals{ 0 };

		struct StateSeen
		{
			std::uint16_t target{ 0xFFFF };
			std::uint8_t  mode{ 0 }, result{ 0 }, flags{ 0 };
			std::uint64_t enterMs{ 0 }, contactMs{ 0 }, exitMs{ 0 };
			std::uint64_t enters{ 0 };
			bool          active{ false };
		};
		std::mutex g_stateLock;
		StateSeen  g_st;

		// ---- the dog's place (game thread) -------------------------------------------------------------------------------
		buddyloc::Finder    g_finder{ "dog" };
		std::optional<Vec3> g_feet;              // this frame's, while confirmed
		std::optional<Vec3> g_pendingShift;      // a treadmill jump taken last frame
		std::string         g_pendingWhy;
		std::uint64_t       g_shifts{ 0 }, g_shiftFails{ 0 }, g_leashes{ 0 };
		std::uint64_t       g_leashAt{ 0 };
		bool                g_placedThisFrame{ false };
		std::uint16_t       g_dogId{ 0xFFFF };
		std::string         g_lastPlace{ "none" };
		std::atomic<std::uint64_t> g_engagedMs{ 0 };
		std::atomic<bool>   g_engaged{ false };
		// Snake staged at an MGSV ladder or ledge (Climb.h): the move that took the dog along to the site (DogLogic.h StagedSpot)
		std::optional<Vec3> g_stagedMove;
		// Each placement of ours is watched for a few frames: MGSV undid a treadmill shift once in 76 (live 2026-11-04, right after a bite:
		// the dog's place back where it had been, 6 m off in Elden Ring), and it warps its dog when Snake is placed far (back home from a
		// ladder or ledge it put the dog 18 m from him, a frame after ours). A jump of more than 3 m from where it was put (it runs 0.4
		// m a frame at most) is undone: put there again (at most 3 times).
		struct PlaceGuard
		{
			Vec3        expect;  // its feet as put (then as it moves on its own)
			int         framesLeft{ 0 };
			int         tries{ 0 };
			const char* why{ "" };
		};
		std::optional<PlaceGuard> g_guard;
		std::uint64_t             g_shiftsRedone{ 0 };
		std::uint64_t       g_stagedAlong{ 0 }, g_stagedBack{ 0 }, g_stagedFails{ 0 };
		// What Elden Ring is shown (DogLogic.h DisplayLimiter): the published place and the draws' offset
		plan::DisplayLimiter g_limiter;
		std::uint32_t        g_displayEpoch{ 0 };
		std::uint64_t        g_displayMs{ 0 };
		bool                 g_heldNow{ false };
		// Wait in Elden Ring's terms (WaitKeepsPlace): the slip (ActorProxy.h SlipObserver) since the Wait began is added to what Elden
		// Ring is shown of the dog (its place and draws, not MGSV's dog: moving a waiting dog's location object again and again ended
		// MGSV's Wait live, 2026-11-04); after the Wait it fades out while the dog runs back.
		bool                 g_waitWas{ false };
		Vec3                 g_waitOffset;
		Vec3                 g_waitBase;   // what was left of the last Wait's offset when this one began
		plan::WaitFade       g_waitFade;   // after the Wait: the offset shrinks with the dog's way to its goal
		float                g_waitOffsetMax{ 0.0f };
		std::uint64_t        g_waitsWatched{ 0 };

		// Console -> game thread
		std::mutex          g_reqLock;
		std::optional<Vec3> g_testShift;
		std::atomic<bool>   g_whereAsked{ false };

		// The texts more than one thread writes or reads (g_where: game thread / console; g_jobLast: Lua thread / the game thread's
		// Snapshot; g_attackDisabledWhy: Lua thread / Snapshot / console) are copied out under this lock, never formatted from in
		// place (review 0.8.0: an assignment reallocating while another thread copied it could crash, as the suppressor's text could).
		std::mutex          g_textLock;
		std::string         g_where{ "not asked yet (!dog where)" };

		// ---- the player's orders and the job (Lua thread) -----------------------------------------------------------------
		std::atomic<int>           g_playerAttack{ -1 };   // the player's attack command (11-15), taken over, for the Lua thread
		std::atomic<int>           g_playerCancel{ 0 };    // the player's own dog command (8, 9 or 16; never Bark) while an order ran: it
		                                                   // ends the order, and after Wait (9) or DD (16) no whistle of ours undoes it
		std::atomic<bool>          g_waiting{ false };     // the player ordered Wait (until a whistle / DD / attack)
		std::atomic<bool>          g_waitingAtOrder{ false };  // ...as it was when the player's attack order came (no pick: it stays)
		std::atomic<int>           g_hostLayer{ -1 };      // the host's kHostDogLayer as last read (-1: never read)
		std::atomic<std::uint64_t> g_passedToMgsv{ 0 };    // attack orders left to MGSV because the host's layer is off
		std::atomic<std::uint64_t> g_playerDogCommands{ 0 };
		struct ConsoleOrder
		{
			int           cmd{ 0 };
			std::uint16_t arg{ 0 };
			bool          asPlayer{ false };  // `!dog player`: as the call menu would give it (kGEvBuddyOrder, the job)
		};
		std::mutex                  g_orderLock;
		std::vector<ConsoleOrder>   g_consoleOrders;
		std::optional<int>          g_styleWrite;
		plan::Job                   g_job;                 // Lua thread only
		std::uint64_t               g_markersUntil{ 0 };   // Lua thread: MGSV's order markers out of the HUD layer until then
		std::atomic<bool>           g_jobBusy{ false };    // its Busy() for the game thread (leash, treadmill) and the command detour
		int                         g_jobCommand{ 12 };
		bool                        g_orderHeld{ false };   // MGSV's order record has targeted the job's soldier since the order
		std::uint64_t               g_orderOffMs{ 0 };      // since when it no longer does (0: it does)
		std::uint64_t               g_jobOrderMs{ 0 };
		std::uint16_t               g_jobSoldier{ 0xFFFF };
		std::uint64_t               g_jobs{ 0 };
		std::array<std::uint64_t, 5> g_phaseCounts{};
		std::string                 g_jobLast{ "none" };
		std::atomic<bool>           g_attackDisabled{ false };  // the alert phase couldn't be held: Attack off this session
		std::string                 g_attackDisabledWhy;

		// ---- capture: the dog's own meshes (any thread) ------------------------------------------------------------------
		std::mutex                 g_meshLock;
		std::vector<std::uint64_t> g_dogMeshes;
		std::atomic<std::uint64_t> g_keptNearProxy{ 0 }, g_learned{ 0 };

		std::mutex  g_describeLock;
		std::string g_describe{ "dog: no frame yet" };
		std::uint32_t g_describeTick{ 0 };

		// ---- the per-frame place trace (`!dog placetrace on|off`; game thread): where the dog is by every source, frame by frame ----
		struct PlaceNote
		{
			std::optional<Vec3> feetRaw;  // its location object's feet as read this frame, before any placement of ours
			std::optional<Vec3> skin;     // its skin's root (the capture's buddy pose)
			Vec3                shift;    // what a placement of ours moved it by this frame (treadmill, leash, console)
			int                 placedBy{ 0 };  // 0 none, 1 treadmill, 2 leash, 3 console, 4 staging
			Vec3                queued;   // a treadmill shift queued this frame (applied at the next)
			bool                queuedAny{ false };
		};
		PlaceNote                 g_note;
		FILE*                     g_traceFile{ nullptr };
		std::uint64_t             g_traceRows{ 0 };
		std::atomic<int>          g_traceWant{ 0 };  // 1: open, -1: close (console -> game thread)
		std::atomic<int>          g_jobStageA{ 0 };  // g_job's stage for the game thread (the trace)
		// The host's pick as last seen before an order (Lua thread; the memory through the click, kPickMemoryMs)
		std::uint64_t              g_pickSeen{ 0 }, g_pickSeenMs{ 0 };
		std::atomic<std::uint64_t> g_pickReads{ 0 }, g_pickMemoryUsed{ 0 }, g_pickMemoryMissed{ 0 };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return a_at ? SafeRead<T>(reinterpret_cast<const void*>(a_at)) : std::nullopt;
		}

		// MGSV's code (.xtext 0x140001000-0x142089000, .ecode 0x149E7F000-0x14A01D000): a vtable slot must point there.
		bool InCode(std::uintptr_t a_fn)
		{
			return (a_fn >= 0x140001000 && a_fn < 0x142089000) || (a_fn >= 0x149E7F000 && a_fn < 0x14A01D000);
		}

		// ---- the trace (`!dog trace on`): every dog action state's enter / exit / changes, the attack decision, the messages sent to
		// the order's target. Observe only; installed at run time.
		using DecideFn = void (*)(std::uint8_t* a_owner, std::uint8_t* a_data, std::uint32_t a_index);
		using SendFn = std::int32_t* (*)(void* a_box, std::int32_t* a_result, std::uint16_t a_target, void* a_message);
		std::array<StateFn, 14>   g_traceOrig{};
		DecideFn                  g_decideOrig{ nullptr };
		SendFn                    g_sendOrig{ nullptr };
		std::atomic<bool>         g_tracing{ false };
		bool                      g_traceInstalled{ false };
		std::atomic<std::uint16_t> g_watchTarget{ 0xFFFF };  // messages sent to this GameObject are logged while tracing
		std::atomic<std::uint64_t> g_traceLines{ 0 };
		struct TraceSeen
		{
			std::uint16_t target{ 0xFFFF };
			std::uint8_t  mode{ 0 }, result{ 0 }, flags{ 0 };
			std::uint64_t enterMs{ 0 };
		};
		std::mutex                 g_traceLock;
		std::array<TraceSeen, 14>  g_traceSeen{};
		std::array<std::uint64_t, 14> g_stateEnters{};

		bool TraceBudget() { return g_traceLines.fetch_add(1, std::memory_order_relaxed) < 3000; }

		void TraceNote(int a_state, std::uint32_t a_event, std::uint8_t* a_data)
		{
			if (!g_tracing.load(std::memory_order_relaxed) || !a_data) {
				return;
			}
			const auto at = reinterpret_cast<std::uintptr_t>(a_data);
			const auto target = Get<std::uint16_t>(at + 0x20).value_or(0xFFFF);
			const auto mode = Get<std::uint8_t>(at + 0x24).value_or(0);
			const auto result = Get<std::uint8_t>(at + 0x25).value_or(0);
			const auto flags = Get<std::uint8_t>(at + 0x27).value_or(0);
			const auto now = ::GetTickCount64();
			std::scoped_lock lock(g_traceLock);
			auto&            t = g_traceSeen[static_cast<std::size_t>(a_state)];
			if (a_event == 0 || a_event == 4) {
				++g_stateEnters[static_cast<std::size_t>(a_state)];
				t = { target, mode, result, flags, now };
				if (TraceBudget()) {
					Log::Info("dog: trace: state {} {} (target {:#x} mode {} result {} flags {:#x})", a_state, a_event ? "re-entered" : "entered", target, mode, result, flags);
				}
			} else if (a_event == 1) {
				if (TraceBudget()) {
					Log::Info("dog: trace: state {} left after {} ms (target {:#x} mode {} result {} flags {:#x})", a_state, now - t.enterMs, target, mode, result, flags);
				}
			} else if (target != t.target || result != t.result || flags != t.flags || mode != t.mode) {
				if (TraceBudget()) {
					Log::Info("dog: trace: state {} +{} ms: target {:#x} mode {} result {} flags {:#x}", a_state, now - t.enterMs, target, mode, result, flags);
				}
				t.target = target;
				t.mode = mode;
				t.result = result;
				t.flags = flags;
			}
		}

		template <int I>
		void TraceDetour(std::uint8_t* a_owner, std::uint32_t a_index, std::uint32_t a_event, std::uint8_t* a_data)
		{
			g_traceOrig[I](a_owner, a_index, a_event, a_data);
			TraceNote(I, a_event, a_data);
		}

		void DecideDetour(std::uint8_t* a_owner, std::uint8_t* a_data, std::uint32_t a_index)
		{
			g_decideOrig(a_owner, a_data, a_index);
			if (g_tracing.load(std::memory_order_relaxed) && a_data && TraceBudget()) {
				const auto at = reinterpret_cast<std::uintptr_t>(a_data);
				Log::Info("dog: trace: the attack decision: target {:#x} mode {} -> result {} (flags {:#x})", Get<std::uint16_t>(at + 0x20).value_or(0xFFFF),
					Get<std::uint8_t>(at + 0x24).value_or(0), Get<std::uint8_t>(at + 0x25).value_or(0), Get<std::uint8_t>(at + 0x27).value_or(0));
			}
		}

		std::int32_t* SendDetour(void* a_box, std::int32_t* a_result, std::uint16_t a_target, void* a_message)
		{
			auto* r = g_sendOrig(a_box, a_result, a_target, a_message);
			if (a_target == g_watchTarget.load(std::memory_order_relaxed) && a_target != 0xFFFF && g_tracing.load(std::memory_order_relaxed) && a_message &&
				TraceBudget()) {
				const auto id = Get<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_message)).value_or(0);
				const auto arg = Get<std::uint16_t>(reinterpret_cast<std::uintptr_t>(a_message) + 8).value_or(0);
				Log::Info("dog: trace: message {:#x} (u16 +8 {:#x}) sent to {:#x} from {:#x} -> result {}", id, arg, a_target,
					reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), a_result ? *a_result : -999);
			}
			return r;
		}

		template <std::size_t... I>
		void InstallTraceStates(std::index_sequence<I...>)
		{
			((I == 5 ? void() : void(Matches(*addr::kDogStates[I], "dog state") &&
			                         Hooks::Create(std::format("dog state {}", I), reinterpret_cast<void*>(addr::kDogStates[I]->va), &TraceDetour<static_cast<int>(I)>,
			                             &g_traceOrig[I]))),
				...);
		}

		std::string StartTrace()
		{
			if (!g_traceInstalled) {
				g_traceInstalled = true;
				// The live state table must hold exactly the functions checked above.
				int matching = 0;
				for (std::size_t i = 0; i < 14; ++i) {
					matching += Get<std::uintptr_t>(addr::kDogStateTable + i * 16).value_or(0) == addr::kDogStates[i]->va;
				}
				if (matching != 14) {
					return std::format("dog: trace refused -- the state table at {:#x} holds {} of the 14 states", addr::kDogStateTable, matching);
				}
				InstallTraceStates(std::make_index_sequence<14>{});
				if (Matches(addr::kDogAttackDecide, "dog attack decision")) {
					Hooks::Create("dog attack decision", reinterpret_cast<void*>(addr::kDogAttackDecide.va), &DecideDetour, &g_decideOrig);
				}
				if (g_boxOk) {
					const auto obj = reinterpret_cast<AccessorFn>(addr::kMessageBoxAccessor.va)();
					const auto box = Get<std::uintptr_t>(obj + addr::kMessageBoxOffset);
					const auto vt = box && *box ? Get<std::uintptr_t>(*box) : std::nullopt;
					const auto fn = vt && *vt ? Get<std::uintptr_t>(*vt + addr::kMessageBoxSendSlot) : std::nullopt;
					if (fn && InCode(*fn)) {
						Hooks::Create("message box send (dog trace)", reinterpret_cast<void*>(*fn), &SendDetour, &g_sendOrig);
						Log::Info("dog: trace: the message box {:#x} sends through {:#x}", *box, *fn);
					}
				}
				Hooks::EnableAll();
			}
			g_traceLines.store(0);
			g_tracing.store(true);
			return "dog: trace on (states, the attack decision, messages to the order's target)";
		}

		const char* MessageName(std::uint64_t a_id)
		{
			switch (a_id) {
			case kMsgWhistle:
				return "whistle";
			case kMsgWait:
				return "wait";
			case kMsgBark:
				return "bark";
			case kMsgAttack:
				return "attack/fetch";
			case kMsgTalk:
				return "DD";
			case addr::kDogTransformQuery:
				return "transform query";
			case buddy::plan::kMsgLeave:
				return "leave";
			default:
				return nullptr;
			}
		}

		bool LayerOn() { return Cfg().dog.enabled && g_sites == 1; }

		bool DogOut(const state::LuaView& a_lua)
		{
			return a_lua.InGame() && a_lua.buddyActive == state::kBuddyTypeDog && a_lua.buddyId != 0xFFFF && (a_lua.buddyId >> 9) == addr::kDogObjectType &&
			       !state::BuddyMissing();
		}

		// ---- the detours (observe only) -----------------------------------------------------------------------------------
		std::int32_t* HandlerDetour(std::uint8_t* a_sys, std::int32_t* a_result, std::uint32_t a_index, std::uint64_t* a_message)
		{
			const std::uint64_t id = a_message ? a_message[0] : 0;
			const std::uint16_t arg = a_message ? *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(a_message) + 8) : 0;
			auto* r = g_handler(a_sys, a_result, a_index, a_message);
			const auto count = Get<std::uint32_t>(reinterpret_cast<std::uintptr_t>(a_sys) + 8);
			if (!count || a_index >= *count) {
				return r;
			}
			g_messages.fetch_add(1, std::memory_order_relaxed);
			if (id == buddy::plan::kMsgLeave) {
				// MGSV's buddy system sends the dog away (the iDroid's DISMISS / CHANGE EQUIPMENT / another buddy; BuddyLogic.h)
				state::NoteBuddyLeave(state::kBuddyTypeDog, ::GetTickCount64());
			}
			if (g_sys.exchange(reinterpret_cast<std::uintptr_t>(a_sys)) != reinterpret_cast<std::uintptr_t>(a_sys)) {
				Log::Info("dog: the dog system is {:#x} (dog index {}, {} dogs)", reinterpret_cast<std::uintptr_t>(a_sys), a_index, *count);
			}
			g_index.store(a_index, std::memory_order_relaxed);
			const char* name = MessageName(id);
			{
				std::scoped_lock lock(g_seenLock);
				g_seen[g_seenAt++ % g_seen.size()] = { id, arg, ::GetTickCount64() };
				if (std::find(g_seenIds.begin(), g_seenIds.end(), id) == g_seenIds.end() && g_seenIds.size() < 64) {
					g_seenIds.push_back(id);
					Log::Info("dog: its handler got message {:#x}{} (arg {:#x}) -- first of its kind", id, name ? std::format(" ({})", name) : std::string(), arg);
				}
			}
			const bool order = id == kMsgWhistle || id == kMsgWait || id == kMsgBark || id == kMsgAttack || id == kMsgTalk;
			if (order) {
				g_orderMessages.fetch_add(1, std::memory_order_relaxed);
				// The order record the handler just wrote: the same getter with the same arguments, right after it.
				std::uintptr_t rec = 0;
				if (g_recordOk) {
					const auto sub = Get<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_sys) + 0x28);
					const auto vt = sub && *sub ? Get<std::uintptr_t>(*sub) : std::nullopt;
					const auto fn = vt && *vt ? Get<std::uintptr_t>(*vt + 0x58) : std::nullopt;
					if (fn && InCode(*fn)) {
						rec = reinterpret_cast<std::uintptr_t>(reinterpret_cast<RecordFn>(*fn)(reinterpret_cast<void*>(*sub), a_index, 0, addr::kDogOrderRecordKey));
					}
				}
				if (rec) {
					g_record.store(rec, std::memory_order_relaxed);
				}
				const auto bits = Get<std::uint32_t>(rec + addr::kDogOrderBits);
				const auto target = Get<std::uint16_t>(rec + addr::kDogOrderTarget);
				const auto style = Get<std::uint8_t>(reinterpret_cast<std::uintptr_t>(a_sys) + addr::kDogStyleByte);
				Log::Info("dog: order {} (arg {:#x}) taken by the dog's handler: style byte {}, order record {:#x} bits {} target {}", name, arg,
					style ? std::to_string(*style) : std::string("?"), rec, bits ? std::format("{:#x}", *bits) : std::string("?"),
					target ? std::format("{:#x}", *target) : std::string("?"));
			}
			return r;
		}

		// The soldier the component's local index stands for: [[[comp+0x78]+0x18]+0x28] + local*2 (the handler's own lookup).
		std::optional<std::uint16_t> SoldierIndex(std::uint8_t* a_component, std::uint32_t a_local)
		{
			const auto p78 = Get<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_component) + 0x78);
			const auto p18 = p78 && *p78 ? Get<std::uintptr_t>(*p78 + 0x18) : std::nullopt;
			const auto p28 = p18 && *p18 ? Get<std::uintptr_t>(*p18 + 0x28) : std::nullopt;
			return p28 && *p28 && a_local < 0x400 ? Get<std::uint16_t>(*p28 + a_local * 2u) : std::nullopt;
		}

		void SoldierDetour(std::uint8_t* a_component, std::uint32_t a_local, std::int32_t* a_result, std::uint64_t* a_message)
		{
			const std::uint64_t id = a_message ? a_message[0] : 0;
			const bool          dogMsg = id == addr::kDogMsgBite || id == addr::kDogMsgKnife || id == addr::kDogMsgStunKnife;
			const std::uint16_t sender = dogMsg ? *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::uint8_t*>(a_message) + 8) : 0;
			if (!dogMsg || !a_result || (sender >> 9) != addr::kDogObjectType) {
				g_soldier(a_component, a_local, a_result, a_message);
				return;
			}
			// The handler writes 0 only when it accepts (refused: no write at all): a sentinel tells the two apart.
			constexpr std::int32_t kSentinel = 0x7EADD06;
			const std::int32_t     before = *a_result;
			*a_result = kSentinel;
			g_soldier(a_component, a_local, a_result, a_message);
			const std::int32_t after = *a_result;
			if (after == kSentinel) {
				*a_result = before;
			}
			// The knife message's +0xA byte is set for the stun knife (style byte 2, "Do it (knock out enemy)"), clear for the knife
			// (4, "Do it (kill enemy)"): live 2026-11-04 with MGSV's own equipment (it had been read as a "lethal" flag).
			const bool stun = id == addr::kDogMsgKnife && reinterpret_cast<const std::uint8_t*>(a_message)[0xA] != 0;
			const auto index = SoldierIndex(a_component, a_local);
			const std::uint16_t soldier = index && *index < 0x200 ? static_cast<std::uint16_t>(0x400 + *index) : 0xFFFF;
			const auto attack = plan::AttackIdOfMessage(id, stun);
			if (after == 0) {
				const auto n = g_accepts.fetch_add(1, std::memory_order_relaxed) + 1;
				{
					std::scoped_lock lock(g_acceptLock);
					g_lastAccept = { soldier, id, stun, ::GetTickCount64() };
				}
				const auto handle = soldier != 0xFFFF ? proxy::HandleOfSoldier(soldier) : 0;
				if (n <= 200) {
					Log::Info("dog: bite connected ({:#x} {} accepted by soldier {:#x}, local {}, from dog {:#x}){}", id,
						attack == 144 ? "bite" : attack == 145 ? "knife, lethal" : attack == 146 ? "knife, not lethal (stun)" : "body attack", soldier, a_local,
						sender, handle ? std::format(" -- he stands in for Elden Ring character {:#x}", handle) : std::string(" -- not a proxy"));
				}
			} else {
				const auto n = g_refusals.fetch_add(1, std::memory_order_relaxed) + 1;
				if (n <= 50) {
					Log::Info("dog: soldier {:#x} refused the dog's {:#x} (result {:#x})", soldier, id, after == kSentinel ? -1 : after);
				}
			}
		}

		void StateDetour(std::uint8_t* a_owner, std::uint32_t a_index, std::uint32_t a_event, std::uint8_t* a_data)
		{
			g_state(a_owner, a_index, a_event, a_data);
			TraceNote(5, a_event, a_data);
			if (!a_data) {
				return;
			}
			const auto at = reinterpret_cast<std::uintptr_t>(a_data);
			const auto target = Get<std::uint16_t>(at + 0x20).value_or(0xFFFF);
			const auto mode = Get<std::uint8_t>(at + 0x24).value_or(0);
			const auto result = Get<std::uint8_t>(at + 0x25).value_or(0);
			const auto flags = Get<std::uint8_t>(at + 0x27).value_or(0);
			const auto now = ::GetTickCount64();
			std::scoped_lock lock(g_stateLock);
			if (a_event == 0 || a_event == 4) {
				g_st.active = true;
				g_st.target = target;
				g_st.mode = mode;
				g_st.result = result;
				g_st.flags = flags;
				g_st.enterMs = now;
				g_st.contactMs = 0;
				++g_st.enters;
				if (g_st.enters <= 200) {
					Log::Info("dog: attack state {} on target {:#x} (mode {}, flags {:#x}){}", a_event == 0 ? "entered" : "re-entered", target, mode, flags,
						flags & 1 ? " -- the target accepted" : (target >> 9) == 2 ? " -- the target did NOT accept" : "");
				}
			} else if (a_event == 3) {
				if ((flags & 2) && !(g_st.flags & 2)) {
					g_st.contactMs = now;
					Log::Info("dog: contact with {:#x} ({} ms after the attack state began)", target, now - g_st.enterMs);
				}
				if (result != g_st.result && result) {
					Log::Info("dog: attack state result {} on {:#x} ({} ms in)", result, target, now - g_st.enterMs);
				}
				g_st.flags = flags;
				g_st.result = result;
			} else if (a_event == 1) {
				g_st.active = false;
				g_st.exitMs = now;
				Log::Info("dog: attack state left (target {:#x}, result {}, flags {:#x}, {} ms)", target, result, flags, now - g_st.enterMs);
			}
		}

		// ---- reads --------------------------------------------------------------------------------------------------------
		std::optional<std::uint8_t> VarU8(const addr::VarSite& a_site)
		{
			if (!g_varsOk) {
				return std::nullopt;
			}
			const auto block = Get<std::uintptr_t>(addr::kVarsPtr);
			return block && *block ? Get<std::uint8_t>(*block + a_site.offset) : std::nullopt;
		}

		// Can the dog take orders now (0x140C56670's check on [[sys+0x20]+8] + index*0x20).
		std::optional<bool> CanOrder()
		{
			const auto sys = g_sys.load();
			const auto p20 = Get<std::uintptr_t>(sys + 0x20);
			const auto rec = p20 && *p20 ? Get<std::uintptr_t>(*p20 + 8) : std::nullopt;
			if (!rec || !*rec) {
				return std::nullopt;
			}
			const auto base = *rec + std::uintptr_t(g_index.load()) * 0x20;
			const auto b1b = Get<std::uint8_t>(base + 0x1B);
			const auto b1d = Get<std::uint8_t>(base + 0x1D);
			if (!b1b || !b1d) {
				return std::nullopt;
			}
			const int low = *b1d & 3;
			return *b1b != 2 && !(low >= 1 && low <= 3);
		}

		// The dog's position cache ([[sys+0x10]+0x20] + index*16), what MGSV answers when it isn't realized.
		std::optional<Vec3> CachePos()
		{
			if (!g_cacheOk) {
				return std::nullopt;
			}
			const auto p10 = Get<std::uintptr_t>(g_sys.load() + 0x10);
			const auto p20 = p10 && *p10 ? Get<std::uintptr_t>(*p10 + 0x20) : std::nullopt;
			const auto v = p20 && *p20 ? Get<std::array<float, 3>>(*p20 + std::uintptr_t(g_index.load()) * 16) : std::nullopt;
			if (!v) {
				return std::nullopt;
			}
			const Vec3 p{ (*v)[0], (*v)[1], (*v)[2] };
			return p.Finite() ? std::optional<Vec3>(p) : std::nullopt;
		}

		// The transform query a soldier sends the dog, sent natively (game thread): the dog's location object's +0x20 (feet + lift).
		std::optional<Vec3> QueryTransform(std::uint16_t a_target, std::array<float, 4>* a_rot)
		{
			if (!g_boxOk) {
				return std::nullopt;
			}
			const auto obj = reinterpret_cast<AccessorFn>(addr::kMessageBoxAccessor.va)();
			const auto box = Get<std::uintptr_t>(obj + addr::kMessageBoxOffset);
			const auto vt = box && *box ? Get<std::uintptr_t>(*box) : std::nullopt;
			const auto fn = vt && *vt ? Get<std::uintptr_t>(*vt + addr::kMessageBoxSendSlot) : std::nullopt;
			if (!fn || !InCode(*fn)) {
				return std::nullopt;
			}
			alignas(16) std::array<std::uint8_t, 0x40> msg{};
			const std::uint64_t id = addr::kDogTransformQuery;
			std::memcpy(msg.data(), &id, 8);
			const float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			std::memcpy(msg.data() + 0x10, q, 16);
			std::int32_t result = -1;
			reinterpret_cast<BoxSendFn>(*fn)(reinterpret_cast<void*>(*box), &result, a_target, msg.data());
			if (msg[0x30] != 1) {
				return std::nullopt;
			}
			float p[3];
			std::memcpy(p, msg.data() + 0x20, 12);
			if (a_rot) {
				std::memcpy(a_rot->data(), msg.data() + 0x10, 16);
			}
			const Vec3 v{ p[0], p[1], p[2] };
			return v.Finite() ? std::optional<Vec3>(v) : std::nullopt;
		}

		std::optional<Vec3> Skin()
		{
			Vec3  p;
			float yaw = 0;
			return skeleton::BuddyPose(p, yaw, 500) ? std::optional<Vec3>(p) : std::nullopt;
		}

		std::uint32_t ShareOf(std::uint32_t a_attack)
		{
			std::uint16_t lethal = 0, nonLethal = 0;
			std::uint32_t share = 0;
			return a_attack && combat::AttackRow(a_attack, lethal, nonLethal, share) ? share : 0;
		}

		// kGEvDogAttack (Lua thread).
		void SendEvent(proto::DogPhase a_phase, proto::DogStyle a_style, std::uint64_t a_handle, std::uint32_t a_attack, const char* a_why)
		{
			proto::GuestEvent e{};
			e.type = proto::kGEvDogAttack;
			e.a = proto::DogAttackA(a_phase, a_style, a_attack);
			e.b = ShareOf(a_attack);
			const auto fr = state::Frame();
			e.frame = static_cast<std::uint32_t>(fr.frame);
			const auto lua = state::Lua();
			const Vec3 dogAt = lua.buddyPosValid ? lua.buddyPos : fr.root;
			e.v0[0] = dogAt.x;
			e.v0[1] = dogAt.y;
			e.v0[2] = dogAt.z;
			const auto ded = proxy::DedicatedState();
			if (a_handle && ded.handle == a_handle && ded.soldier != 0xFFFF) {
				const Vec3  d = ded.real - dogAt;
				const float len = d.Length();
				if (len > 0.05f) {
					e.v1[0] = d.x / len;
					e.v1[1] = d.y / len;
					e.v1[2] = d.z / len;
				}
			}
			e.pad[0] = static_cast<std::uint32_t>(a_handle & 0xFFFFFFFFu);
			e.pad[1] = static_cast<std::uint32_t>(a_handle >> 32);
			const bool sent = link::PushEvent(e);
			++g_phaseCounts[std::min<std::size_t>(a_phase, g_phaseCounts.size() - 1)];
			{
				auto             last = std::format("{} {} on {:#x} ({})", plan::PhaseName(a_phase), plan::StyleName(a_style), a_handle, a_why);
				std::scoped_lock lock(g_textLock);
				g_jobLast = std::move(last);
			}
			Log::Info("dog: kGEvDogAttack {} style {} (MGSV attack {}, share {}) on Elden Ring {:#x}: {}{}", plan::PhaseName(a_phase), plan::StyleName(a_style),
				a_attack, e.b, a_handle, a_why, sent ? "" : " -- event ring full, NOT sent");
		}

		void Snapshot();

		// The host's D-Dog layer (HostState kHostDogLayer, v17): only while it is on does an attack order go to an Elden Ring enemy --
		// without it the host picks no one, and an order taken over would end Refused and whistled back with nothing on screen (review
		// 0.8.0). A torn read keeps the last answer; no host = off.
		bool HostLayer()
		{
			int now = -1;
			if (!link::HostAlive()) {
				now = 0;
			} else if (proto::HostState hs{}; link::ReadHostState(hs)) {
				now = (hs.flags & proto::kHostDogLayer) ? 1 : 0;
			}
			if (now < 0) {
				return g_hostLayer.load() == 1;
			}
			if (g_hostLayer.exchange(now) != now) {
				Log::Info("dog: the host's D-Dog layer is {}", now ? "on (kHostDogLayer): attack orders go to the Elden Ring enemy it picks"
				                                                   : "off: attack orders keep MGSV's own meaning");
			}
			return now == 1;
		}
	}

	// ======================================================================================================================
	void Install()
	{
		const bool handler = Matches(addr::kDogMessageHandler, "dog message handler") && Matches(addr::kDogStyleRead, "dog style read") &&
		                     Matches(addr::kDogAttackOrderStore, "dog attack order store") && Matches(addr::kDogCanOrder, "dog can-order check");
		const bool soldier = Matches(addr::kSoldierDogAttack, "soldier dog-attack handler") && Matches(addr::kSoldierDogStunKnife, "soldier dog stun knife") &&
		                     Matches(addr::kSoldierDogKnife, "soldier dog knife") && Matches(addr::kSoldierDogBite, "soldier dog bite") &&
		                     Matches(addr::kSoldierDogIndexLookup, "soldier dog index lookup") && Matches(addr::kSoldierDogAccept, "soldier dog accept");
		const bool state = Matches(addr::kDogAttackState, "dog attack state") && Matches(addr::kDogAttackStateSend, "dog attack state send") &&
		                   Matches(addr::kDogAttackStateBox, "dog attack state box");
		const bool commands = Matches(addr::kBuddyCommandDogWhistle, "buddy command: dog whistle") && Matches(addr::kBuddyCommandDogWait, "buddy command: dog wait") &&
		                      Matches(addr::kBuddyCommandDogAttack, "buddy command: dog attack") && Matches(addr::kBuddyCommandDogTalk, "buddy command: dog DD") &&
		                      Matches(addr::kBuddyCommandDogBark, "buddy command: dog bark");
		g_boxOk = Matches(addr::kMessageBoxAccessor, "message box accessor") && state;
		g_recordOk = Matches(addr::kDogOrderRecordCall, "dog order record call");
		g_cacheOk = Matches(addr::kDogCacheBase, "dog position cache base") && Matches(addr::kDogCacheRow, "dog position cache row");
		g_varsOk = Matches(addr::kVarBuddyDogEquipType) && Matches(addr::kVarBuddyDogMode) && Matches(addr::kVarBuddyDogFlag);
		g_sites = handler && soldier && state && commands ? 1 : 0;
		if (!g_sites) {
			Log::Warn("dog: MGSV's dog code differs from what this build was written for -- the D-Dog layer stays off ([Dog] ignored)");
			return;
		}
		g_hooked[0] = Hooks::Create("dog message handler", reinterpret_cast<void*>(addr::kDogMessageHandler.va), &HandlerDetour, &g_handler);
		g_hooked[1] = Hooks::Create("soldier dog-attack handler", reinterpret_cast<void*>(addr::kSoldierDogAttack.va), &SoldierDetour, &g_soldier);
		g_hooked[2] = Hooks::Create("dog attack state", reinterpret_cast<void*>(addr::kDogAttackState.va), &StateDetour, &g_state);
		if (!g_hooked[0] || !g_hooked[1]) {
			g_sites = 0;
			Log::Warn("dog: a detour was refused (handler {}, soldier {}) -- the D-Dog layer stays off", g_hooked[0], g_hooked[1]);
			return;
		}
		const auto& d = Cfg().dog;
		Log::Info("dog: sites ok (order record {}, position cache {}, message box {}, vars {}); [Dog] Enabled {} Treadmill {} Attack {} TargetPuppet {} "
		          "KeepDrawsNearProxy {} WhistleBack {}",
			g_recordOk, g_cacheOk, g_boxOk, g_varsOk, d.enabled, d.treadmill, d.attack, d.targetPuppet, d.keepDrawsNearProxy, d.whistleBack);
	}

	// ---- game thread ---------------------------------------------------------------------------------------------------------
	void Update(const state::LuaView& a_lua, const FrameIn& a_in)
	{
		if (++g_describeTick % 15 == 0) {
			Snapshot();
		}
		std::optional<Vec3> test;
		{
			std::scoped_lock lock(g_reqLock);
			test = std::exchange(g_testShift, std::nullopt);
		}
		g_feet.reset();
		g_placedThisFrame = false;
		g_note = {};
		const bool out = g_sites == 1 && DogOut(a_lua);
		if (!out && g_stagedMove) {
			g_stagedMove.reset();  // the dog went away meanwhile: nothing to bring back
		}
		if (out && a_lua.buddyId != g_dogId) {
			g_finder.Forget("another dog");
			g_dogId = a_lua.buddyId;
			g_pendingShift.reset();
		}
		const auto skin = out ? Skin() : std::nullopt;

		// `!dog where`: the transform query vs the skin vs the position cache (works with the layer off: read-only).
		if (out && g_whereAsked.exchange(false)) {
			std::array<float, 4> rot{};
			const auto           q = QueryTransform(static_cast<std::uint16_t>(0x2600 | (g_index.load() & 0x1FF)), &rot);
			const auto           cache = CachePos();
			const auto           loc = g_finder.Confirmed();
			const auto           feet = loc ? buddyloc::Finder::Feet(loc) : std::nullopt;
			const auto           point = loc ? Get<std::array<float, 3>>(loc + 0x20) : std::nullopt;
			auto where = std::format("transform query {} | skin {} | position cache {} | location object {:#x} feet {} +0x20 {}",
				q ? std::format("{:.2f},{:.2f},{:.2f} (rot {:.2f},{:.2f},{:.2f},{:.2f})", q->x, q->y, q->z, rot[0], rot[1], rot[2], rot[3]) : std::string("none"),
				skin ? std::format("{:.2f},{:.2f},{:.2f}", skin->x, skin->y, skin->z) : std::string("none"),
				cache ? std::format("{:.2f},{:.2f},{:.2f}", cache->x, cache->y, cache->z) : std::string("none"), loc,
				feet ? std::format("{:.2f},{:.2f},{:.2f}", feet->x, feet->y, feet->z) : std::string("-"),
				point ? std::format("{:.2f},{:.2f},{:.2f}", (*point)[0], (*point)[1], (*point)[2]) : std::string("-"));
			if (q && skin) {
				where += std::format(" | query - skin {:.2f} m (xz {:.2f})", (*q - *skin).Length(), (*q - *skin).LengthXZ());
			}
			Log::Info("dog: where: {}", where);
			std::scoped_lock lock(g_textLock);
			g_where = std::move(where);
		}

		const auto& d = Cfg().dog;
		static bool holdTried = false;
		if (out && LayerOn() && !hold::Available() && !holdTried) {
			holdTried = true;  // the location setter lives there (byte checks only; once)
			hold::Init();
		}
		const bool placeable = out && LayerOn() && (d.treadmill || test) && buddyloc::SitesOk("dog") && hold::Available();
		g_finder.Update(skin, a_in.nowMs, placeable);
		const auto loc = placeable ? g_finder.Confirmed() : 0;
		auto       feet = loc ? buddyloc::Finder::Feet(loc) : std::nullopt;
		g_note.feetRaw = feet;
		g_note.skin = skin;

		if (g_guard && feet && !g_pendingShift) {
			auto& gd = *g_guard;
			const float off = (*feet - gd.expect).LengthXZ();
			if (off > 3.0f && gd.tries < 3) {
				// MGSV moved it away from where we put it (undid the move, or warped it): there again, its height as MGSV has it.
				const Vec3 to{ gd.expect.x, feet->y, gd.expect.z };
				if (g_finder.Place(*feet, to, std::nullopt)) {
					++g_shiftsRedone;
					if (g_shiftsRedone <= 20) {
						Log::Info("dog: MGSV moved it {:.1f} m from where it was put ({}) -- put there again", off, gd.why);
					}
					g_note.shift = to - *feet;
					g_note.placedBy = 6;
					g_placedThisFrame = true;
					feet = to;
				}
				++gd.tries;
			} else {
				gd.expect = *feet;  // its own steps
			}
			if (--gd.framesLeft <= 0) {
				g_guard.reset();
			}
		}
		if (const auto shift = std::exchange(g_pendingShift, std::nullopt)) {
			// Last frame's treadmill jump, now that Snake's root has landed: the dog by the same shift.
			if (feet && g_finder.Place(*feet, *feet + *shift, std::nullopt)) {
				++g_shifts;
				feet = *feet + *shift;
				g_placedThisFrame = true;
				g_note.shift = *shift;
				g_note.placedBy = 1;
				g_guard = PlaceGuard{ *feet, 3, 0, "a treadmill shift" };
				Log::Debug("dog: moved by {:.2f},{:.2f},{:.2f} ({})", shift->x, shift->y, shift->z, g_pendingWhy);
			} else {
				++g_shiftFails;
				if (g_shiftFails <= 20) {
					Log::Warn("dog: {}: its location object couldn't be moved (MGSV brings it back itself)", g_pendingWhy);
				}
			}
		}
		if (test) {
			if (!feet) {
				g_lastPlace = "console: not moved (no confirmed object)";
			} else {
				const bool ok = g_finder.Place(*feet, *feet + Vec3{ test->x, 0.0f, test->z }, std::nullopt);
				g_lastPlace = std::format("console: {} by {:.2f},{:.2f} from {:.2f},{:.2f},{:.2f}", ok ? "moved" : "NOT moved", test->x, test->z, feet->x, feet->y, feet->z);
				Log::Info("dog: {}", g_lastPlace);
				if (ok) {
					feet = *feet + Vec3{ test->x, 0.0f, test->z };
					g_placedThisFrame = true;
					g_note.shift = g_note.shift + Vec3{ test->x, 0.0f, test->z };
					g_note.placedBy = 3;
				}
			}
		}

		// Snake staged at an MGSV ladder or ledge (Climb.h): a free dog goes along with him -- put behind him at the site once his root
		// shows there, put back by the same move once he is home (MGSV ran after him and then warped it to him: an 84 m jump live).
		if (feet && !g_placedThisFrame) {
			const bool free = !g_waiting.load() && !g_jobBusy.load();
			if (!g_stagedMove && d.stageAlong && a_in.staged && a_in.onFoot && free && plan::AtSite(a_in.root, a_in.home)) {
				const Vec3 spot = plan::StagedSpot(a_in.root, a_in.yaw);
				const Vec3 to{ spot.x, a_in.root.y, spot.z };
				if (g_finder.Place(*feet, to, std::nullopt)) {
					g_stagedMove = to - *feet;
					++g_stagedAlong;
					g_note.shift = *g_stagedMove;
					g_note.placedBy = 4;
					g_placedThisFrame = true;
					g_lastPlace = std::format("staged: taken along to Snake at the site ({:.1f} m)", g_stagedMove->LengthXZ());
					Log::Info("dog: {}", g_lastPlace);
					feet = to;
					g_guard = PlaceGuard{ to, 45, 0, "taken along to the site" };
				} else if (++g_stagedFails <= 20) {
					Log::Info("dog: staged: its location object couldn't be moved to Snake's site -- MGSV brings it");
				}
			} else if (g_stagedMove && (!a_in.staged || !plan::AtSite(a_in.root, a_in.home))) {
				const Vec3 to = *feet - *g_stagedMove;
				if (g_finder.Place(*feet, to, std::nullopt)) {
					++g_stagedBack;
					g_note.shift = Vec3{} - *g_stagedMove;
					g_note.placedBy = 4;
					g_placedThisFrame = true;
					g_lastPlace = std::format("staged: back home with Snake ({:.1f} m)", g_stagedMove->LengthXZ());
					Log::Info("dog: {}", g_lastPlace);
					feet = to;
					g_guard = PlaceGuard{ to, 45, 0, "back home with Snake" };
				} else if (++g_stagedFails <= 20) {
					Log::Info("dog: staged: its location object couldn't be moved back -- MGSV brings it");
				}
				g_stagedMove.reset();
			}
		} else if (!feet && g_stagedMove && !a_in.staged) {
			g_stagedMove.reset();  // its location object went away meanwhile
		}

		// Wait in Elden Ring's terms: where Elden Ring's walls stopped Snake and MGSV's sandbox didn't (the slip), Elden Ring is shown the
		// waiting dog that much further on in MGSV (render-only: its place and draws), so it stays where Elden Ring saw it wait (it drifted
		// ~3 m live, 0.8.0). After the Wait the difference shrinks with the dog's way to its goal (DogLogic.h WaitFade: Snake, or the
		// soldier of an attack order), so a bite from Wait lands on its enemy -- at 4 m/s it was still metres off when the dog leapt
		// (review 0.8.0); gone once it holds him. A Wait again keeps what is left of it (MGSV's dog never moved meanwhile).
		{
			const bool waiting = out && LayerOn() && d.waitKeepsPlace && g_waiting.load() && !g_jobBusy.load();
			if (waiting && !g_waitWas) {
				proxy::SlipBegin();
				g_waitBase = g_waitOffset;
				++g_waitsWatched;
			} else if (!waiting && g_waitWas) {
				proxy::SlipStop();
			}
			g_waitWas = waiting;
			if (waiting) {
				g_waitOffset = g_waitBase + proxy::Slip();
				g_waitOffsetMax = std::max(g_waitOffsetMax, g_waitOffset.LengthXZ());
				g_waitFade = {};
			} else if (!LayerOn()) {
				g_waitOffset = {};
				g_waitFade = {};
			} else if (g_waitOffset.LengthXZ() > 0.0f) {
				plan::WaitFade::Goal goal;
				const auto           dogAt = feet ? feet : skin;
				if (g_heldNow || g_engaged.load()) {
					goal.kind = plan::WaitFade::kHolding;
				} else if (g_jobBusy.load()) {
					const auto ded = proxy::DedicatedState();
					goal.kind = ded.soldier != 0xFFFF ? plan::WaitFade::kSoldier : plan::WaitFade::kNone;  // no soldier yet: the dog sits on
					goal.at = ded.pos;
				} else {
					goal.kind = plan::WaitFade::kSnake;
					goal.at = a_in.root;
				}
				g_waitOffset = g_waitFade.Step(g_waitOffset, dogAt, goal, a_in.dt);
			}
		}

		// The attack has the dog's soldier (accepted, from the lunge to the let-go): MGSV moves the two together now -- ActorProxy only
		// moves him with the treadmill (he used to be put back onto the Elden Ring character's place, and the dog snapped with him).
		{
			bool held = false;
			if (out && g_jobBusy.load()) {
				StateSeen st;
				{
					std::scoped_lock lock(g_stateLock);
					st = g_st;
				}
				if (st.active && (st.flags & 1)) {
					const auto ded = proxy::DedicatedState();
					held = ded.soldier != 0xFFFF && st.target == ded.soldier;
				}
			}
			if (held != g_heldNow) {
				g_heldNow = held;
				Log::Debug("dog: {} its soldier", held ? "holds" : "let go of");
			}
			proxy::HoldDedicated(held);
		}

		// The leash: a free dog far behind, out of view, put beside Snake out of view (D-Walker's catch-up rule).
		if (feet && a_in.ready && !g_placedThisFrame && !g_waiting.load() && !g_jobBusy.load() && d.leash > 0.0f && a_in.nowMs - g_leashAt >= 1500) {
			const auto view = plan::ViewOf(a_in.camPos, a_in.root);
			if (const auto to = plan::LeashSpot(a_in.root, a_in.velocity, view, *feet, d.leash)) {
				const float gap = (*feet - a_in.root).LengthXZ();
				if (g_finder.Place(*feet, { to->x, a_in.root.y, to->z }, logic::YawQuat(std::atan2(a_in.velocity.x, a_in.velocity.z)))) {
					++g_leashes;
					g_leashAt = a_in.nowMs;
					g_placedThisFrame = true;
					g_lastPlace = std::format("leash: {:.1f} m behind, out of view -> {:.1f} m from Snake", gap, (*to - a_in.root).LengthXZ());
					Log::Info("dog: {}", g_lastPlace);
					g_note.shift = Vec3{ to->x, a_in.root.y, to->z } - *feet;
					g_note.placedBy = 2;
					feet = Vec3{ to->x, a_in.root.y, to->z };
				}
			}
		}
		if (feet) {
			g_feet = feet;
		}
	}

	bool Shift(const Vec3& a_shift, const char* a_why)
	{
		const auto& d = Cfg().dog;
		const auto  loc = g_finder.Confirmed();
		if (!LayerOn() || !d.treadmill || !loc || !a_shift.Finite()) {
			return false;
		}
		const auto  fr = state::Frame();
		const auto  feet = buddyloc::Finder::Feet(loc);
		plan::TreadmillInput in;
		in.enabled = true;
		in.located = feet.has_value();
		in.waiting = g_waiting.load();
		in.engaged = g_jobBusy.load();
		in.distance = feet && fr.valid ? (*feet - fr.root).LengthXZ() : 1e9f;
		in.maxDistance = d.treadmillMaxDistance;
		in.waitMax = 4.0f * d.treadmillMaxDistance;
		if (!plan::MoveAlong(in)) {
			return false;
		}
		g_pendingShift = g_pendingShift ? *g_pendingShift + a_shift : a_shift;
		g_pendingWhy = a_why;
		g_note.queued = g_note.queued + a_shift;
		g_note.queuedAny = true;
		return true;
	}

	void TraceFrame(std::uint64_t a_frame, const Vec3& a_root, const double a_odo[3], const float a_pub[3], const Vec3& a_raw, bool a_fromLocation,
		std::uint32_t a_guestFlags, std::uint32_t a_buddyFlags)
	{
		if (const int want = g_traceWant.exchange(0)) {
			if (want > 0 && !g_traceFile) {
				const auto path = ModuleDir() + L"PhantomRingDogTrace.csv";
				g_traceFile = _wfopen(path.c_str(), L"ab");
				g_traceRows = 0;
				if (g_traceFile) {
					std::fputs("# frame,ms,gflags,bflags,staged,root.x,root.y,root.z,odo.x,odo.y,odo.z,pub.x,pub.y,pub.z,src,feet.x,feet.y,feet.z,skin.x,"
					           "skin.y,skin.z,lua.x,lua.y,lua.z,cache.x,cache.y,cache.z,conf,placedBy,sh.x,sh.y,sh.z,queued,q.x,q.z,st.active,st.flags,"
					           "st.result,st.target,stage,engaged,waiting,ded.soldier,ded.pos.x,ded.pos.y,ded.pos.z,ded.at.x,ded.at.y,ded.at.z,ded.placed,"
					           "ded.ready,raw.x,raw.y,raw.z,held\n",
						g_traceFile);
				}
				Log::Info("dog: place trace {}", g_traceFile ? "on (PhantomRingDogTrace.csv next to the DLL, one line per frame)" : "could NOT be opened");
			} else if (want < 0 && g_traceFile) {
				std::fclose(g_traceFile);
				g_traceFile = nullptr;
				Log::Info("dog: place trace off ({} lines)", g_traceRows);
			}
		}
		if (!g_traceFile) {
			return;
		}
		const auto lua = state::Lua();
		const auto cache = g_sys.load() ? CachePos() : std::nullopt;
		StateSeen  st;
		{
			std::scoped_lock lock(g_stateLock);
			st = g_st;
		}
		const auto ded = proxy::DedicatedState();
		const auto v3 = [](const std::optional<Vec3>& a_v) {
			return a_v ? std::format("{:.3f},{:.3f},{:.3f}", a_v->x, a_v->y, a_v->z) : std::string(",,");
		};
		const auto line = std::format("{},{},{:#x},{:#x},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f},{},{:.3f},{:.3f},{},{:#x},{},{:#x},{},{},{},{:#x},{},{},{},{},{:.3f},{:.3f},{:.3f},{}\n",
			a_frame, ::GetTickCount64(), a_guestFlags, a_buddyFlags, state::Staged() ? 1 : 0, a_root.x, a_root.y, a_root.z, a_odo[0], a_odo[1], a_odo[2], a_pub[0],
			a_pub[1], a_pub[2], a_fromLocation ? 1 : 0, v3(g_note.feetRaw), v3(g_note.skin), v3(lua.buddyPosValid ? std::optional<Vec3>(lua.buddyPos) : std::nullopt),
			v3(cache), g_finder.Confirmed() ? 1 : 0, g_note.placedBy, g_note.shift.x, g_note.shift.y, g_note.shift.z, g_note.queuedAny ? 1 : 0, g_note.queued.x,
			g_note.queued.z, st.active ? 1 : 0, st.flags, st.result, st.target, g_jobStageA.load(), g_engaged.load() ? 1 : 0, g_waiting.load() ? 1 : 0, ded.soldier,
			v3(ded.soldier != 0xFFFF ? std::optional<Vec3>(ded.pos) : std::nullopt), v3(ded.atValid ? std::optional<Vec3>(ded.at) : std::nullopt), ded.placed ? 1 : 0,
			ded.ready ? 1 : 0, a_raw.x, a_raw.y, a_raw.z, g_heldNow ? 1 : 0);
		std::fputs(line.c_str(), g_traceFile);
		if (++g_traceRows % 60 == 0) {
			std::fflush(g_traceFile);
		}
		if (g_traceRows >= 400000) {  // ~1.8 hours at 60 fps: enough
			std::fclose(g_traceFile);
			g_traceFile = nullptr;
			Log::Info("dog: place trace off (full: {} lines)", g_traceRows);
		}
	}

	std::optional<Vec3> Position() { return g_feet; }

	Vec3 StagedMove() { return g_stagedMove.value_or(Vec3{}); }

	Vec3 Display(const Vec3& a_place, const Vec3& a_root, const double a_odo[3], std::uint32_t a_epoch, float a_dt)
	{
		const auto now = ::GetTickCount64();
		// Our own placements Elden Ring isn't meant to see move (the leash, out of view; the console; the staging moves) and a new
		// odometer epoch start over; so does a dog that wasn't shown for a while.
		const auto& d = Cfg().dog;
		const bool  reset = a_epoch != g_displayEpoch || now - g_displayMs > 250 || (g_note.placedBy >= 2 && g_note.placedBy <= 4) || !(d.maxShownSpeed > 0.0f) ||
			!LayerOn();
		g_displayEpoch = a_epoch;
		g_displayMs = now;
		if (!LayerOn()) {
			// [Dog] Enabled = false: D-Dog as in 0.7.0 -- MGSV's place as it is, no limiter, no Wait offset (review 0.8.0).
			g_waitOffset = {};
			return a_place;
		}
		// (+ the Wait's slip: where Elden Ring saw the dog wait)
		const Vec3 real{ static_cast<float>(a_odo[0] + (a_place.x - a_root.x)) + g_waitOffset.x, a_place.y,
			static_cast<float>(a_odo[2] + (a_place.z - a_root.z)) + g_waitOffset.z };
		const Vec3 shown = g_limiter.Step(real, a_dt, reset, { .maxSpeed = std::max(d.maxShownSpeed, 1.0f) });
		return { static_cast<float>(a_root.x + (shown.x - a_odo[0])), a_place.y, static_cast<float>(a_root.z + (shown.z - a_odo[2])) };
	}

	bool Engaged() { return g_engaged.load(std::memory_order_relaxed); }

	// ---- the player's commands (Follow.cpp's detour) ---------------------------------------------------------------------------
	bool OnPlayerCommand(int a_command, std::uint16_t a_arg)
	{
		if (!plan::IsDogCommand(a_command) || g_sites != 1) {
			return false;
		}
		g_playerDogCommands.fetch_add(1, std::memory_order_relaxed);
		if (plan::IsAttackCommand(a_command)) {
			g_waitingAtOrder.store(g_waiting.load());
		}
		if (a_command == plan::kCmdWait) {
			g_waiting.store(true);
		} else if (a_command == plan::kCmdWhistle || a_command == plan::kCmdTalk || plan::IsAttackCommand(a_command)) {
			g_waiting.store(false);
		}
		if (!plan::IsAttackCommand(a_command)) {
			if (g_jobBusy.load() && a_command != plan::kCmdBark) {
				g_playerCancel.store(a_command);
			}
			return false;
		}
		const auto& d = Cfg().dog;
		const auto  lua = state::Lua();
		if (!LayerOn() || !d.attack || g_attackDisabled.load() || !DogOut(lua)) {
			return false;  // MGSV's own meaning (its target is whatever its view centre holds)
		}
		if (!HostLayer()) {
			// The host picks no one ([Dog] enabled = false there): MGSV's own order, as in 0.7.0 -- never a silent whistle-back.
			if (g_passedToMgsv.fetch_add(1, std::memory_order_relaxed) < 20) {
				Log::Info("dog: the player's attack command {} left to MGSV (the host's D-Dog layer is off)", a_command);
			}
			return false;
		}
		if (g_jobBusy.load()) {
			Log::Info("dog: the player's attack command {} (arg {:#x}) while an order runs -- ignored", a_command, a_arg);
			return true;
		}
		g_playerAttack.store(a_command);
		Log::Info("dog: the player's attack command {} (MGSV's own target arg {:#x}) taken over -- for the Elden Ring enemy the host picks", a_command, a_arg);
		return true;
	}

	std::uint32_t OrderStylePad(int a_command)
	{
		if (!plan::IsAttackCommand(a_command)) {
			return 0;
		}
		// The command's own style (MGSV's call menu labels the attack entry by the equipment), else the byte's.
		const auto style = plan::StyleOfCommand(a_command).value_or(plan::StyleOf(g_styleByte.load()));
		return style == proto::kDogStyleUnknown ? 0u : static_cast<std::uint32_t>(style) + 1u;
	}

	std::uint32_t BuddyStyleBits() { return g_sites == 1 ? proto::BuddyDogStyleBits(plan::StyleOf(g_styleByte.load())) : 0u; }

	// ---- the Lua thread ----------------------------------------------------------------------------------------------------------
	void LuaTick(std::uint64_t a_nowMs, bool a_ready)
	{
		if (g_sites != 1) {
			return;
		}
		// The dog system and its order record belong to the dog that is out: while none is (a new equipment's supply drop, another
		// buddy, a dismissal) MGSV frees them -- live 2026-11-04 the style byte read 400 garbage values in the 22 s of a re-deploy and
		// the host's style flipped with them. Forgotten until the next dog takes a message (the handler learns them again).
		static bool forgotten = false;
		if (!DogOut(state::Lua())) {
			if ((g_sys.exchange(0) | g_record.exchange(0)) && !forgotten) {
				Log::Info("dog: D-Dog is not out -- its dog system and order record are forgotten until it takes a message again");
			}
			forgotten = true;
			g_styleByte.store(-1);
		} else {
			forgotten = false;
		}
		const auto sys = g_sys.load();
		if (sys) {
			if (const auto s = Get<std::uint8_t>(sys + addr::kDogStyleByte)) {
				const int was = g_styleByte.exchange(*s);
				if (was != *s) {
					Log::Info("dog: its style byte is {} ({}) -- equipment var {}, mode var {}, flag var {}", *s, plan::StyleName(plan::StyleOf(*s)),
						VarU8(addr::kVarBuddyDogEquipType).value_or(255), VarU8(addr::kVarBuddyDogMode).value_or(255), VarU8(addr::kVarBuddyDogFlag).value_or(255));
				}
			}
		}
		// Wait as MGSV holds it: the order record's bit 2 (cleared by a whistle, an attack, DD). Its changes are logged while tracing.
		if (const auto rec = g_record.load()) {
			const auto bits = Get<std::uint32_t>(rec + addr::kDogOrderBits);
			const auto target = Get<std::uint16_t>(rec + addr::kDogOrderTarget);
			if (bits) {
				g_waiting.store((*bits & 0x4) != 0);
			}
			static std::uint32_t lastBits = 0;
			static std::uint16_t lastTarget = 0;
			if (bits && target && (*bits != lastBits || *target != lastTarget)) {
				if (g_tracing.load() && TraceBudget()) {
					Log::Info("dog: trace: order record bits {:#x} -> {:#x}, target {:#x} -> {:#x}", lastBits, *bits, lastTarget, *target);
				}
				lastBits = *bits;
				lastTarget = *target;
			}
		}
		{
			std::vector<ConsoleOrder> orders;
			std::optional<int>        style;
			{
				std::scoped_lock lock(g_orderLock);
				orders.swap(g_consoleOrders);
				style = std::exchange(g_styleWrite, std::nullopt);
			}
			if (style && sys) {
				const std::uint8_t b = static_cast<std::uint8_t>(*style);
				SIZE_T             put = 0;
				const bool         ok = ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(sys + addr::kDogStyleByte), &b, 1, &put) && put == 1;
				Log::Info("dog: style byte set to {} (console test) {}", *style, ok ? "" : "-- write FAILED");
			}
			for (const auto& o : orders) {
				if (plan::IsAttackCommand(o.cmd)) {
					g_watchTarget.store(o.arg);
				}
				const bool ok = follow::SendCommand(o.cmd, o.arg, "dog console", o.asPlayer);
				Log::Info("dog: console order: buddy command {} arg {:#x}{} {}", o.cmd, o.arg, o.asPlayer ? " as the player's" : "",
					ok ? "sent" : "NOT sent (follow's checks)");
			}
		}

		// ---- the attack job ----
		const auto& d = Cfg().dog;
		const auto  lua = state::Lua();
		const bool  out = DogOut(lua);
		const bool  on = LayerOn() && d.attack && !g_attackDisabled.load();
		g_engaged.store(g_job.Engaged() && out);
		if (!on || !out || !a_ready) {
			// MGSV's markers are shown again at once: the flag's only writer is the tail below, which this return skips (they stayed
			// hidden for every buddy, the rest of the session, once a dismissal, a load or DisableAttack came mid-order -- review 0.8.0).
			g_markersUntil = 0;
			capture::hud::SetHideDogMarkers(false);
			g_playerAttack.store(-1);
			if (g_job.Busy()) {
				const auto step = g_job.Abort(!out ? "the dog is not out" : !a_ready ? "MGSV not ready" : "[Dog] Attack off");
				if (step.event) {
					SendEvent(*step.event, g_job.Style(), g_job.Handle(), plan::AttackIdOf(g_job.Style()), step.why);
				}
				proxy::Release("dog order aborted");
			}
			g_engaged.store(false);
			g_jobBusy.store(false);
			g_jobStageA.store(0);
			return;
		}

		// The host's D-Dog layer, now and then (the pick is read meanwhile only while it is on).
		if (static std::uint64_t layerAt = 0; a_nowMs - layerAt >= 500) {
			layerAt = a_nowMs;
			HostLayer();
		}

		// The host's table: a read the host's writer tore (the seqlock gave up) keeps the last good one -- live, a torn read 2 s into a
		// "Get it" read as "the target is gone" (Lost).
		static std::vector<proto::ActorRecord> recs(proto::kMaxActors), fresh(proto::kMaxActors);
		static std::uint32_t                   n = 0;
		static std::uint64_t                   tableMs = 0;
		std::uint64_t&                         pickSeen = g_pickSeen;     // the host's pick before the order (kPickMemoryMs)
		std::uint64_t&                         pickSeenMs = g_pickSeenMs;
		const int                              cmd = g_playerAttack.exchange(-1);
		// Read while an order needs it, and every 100 ms meanwhile while the host's layer is on (the pick it shows the paw over).
		if (cmd > 0 || g_job.Busy() || (a_nowMs - tableMs >= 100 && g_hostLayer.load() == 1)) {
			std::uint32_t fn = 0, seq = 0;
			if (link::ReadActors(fresh.data(), proto::kMaxActors, fn, seq)) {
				recs.swap(fresh);
				n = fn;
				tableMs = a_nowMs;
				g_pickReads.fetch_add(1, std::memory_order_relaxed);
				if (cmd <= 0 && !g_job.Busy()) {
					for (std::uint32_t i = 0; i < n; ++i) {
						if ((recs[i].flags & proto::kActorDogTarget) && recs[i].handle) {
							pickSeen = recs[i].handle;
							pickSeenMs = a_nowMs;
							break;
						}
					}
				}
			} else if (!link::HostAlive() || a_nowMs - tableMs > 2000) {
				n = 0;  // no host (or no good read for 2 s): nothing to go on
			}
		}
		const auto findRecord = [&](std::uint64_t a_handle) -> const proto::ActorRecord* {
			for (std::uint32_t i = 0; i < n; ++i) {
				if (recs[i].handle == a_handle) {
					return &recs[i];
				}
			}
			return nullptr;
		};

		// The pick wants a fresh table: a torn read right at the order waits a tick or two (at most ~0.5 s) instead of refusing.
		static int           pickRetries = 0;
		static std::uint64_t orderSeenMs = 0;
		if (cmd > 0 && !g_job.Busy() && !orderSeenMs) {
			orderSeenMs = std::max<std::uint64_t>(a_nowMs, 1);
		}
		const auto findPick = [&](bool& a_asleep) -> std::uint64_t {
			// The host's pick: hostile, alive (never a friendly / neutral character: the guest checks the flags again), at most one.
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto& r = recs[i];
				if ((r.flags & proto::kActorDogTarget) && (r.flags & proto::kActorHostile) && !(r.flags & proto::kActorDead) && r.handle) {
					a_asleep = (r.flags & proto::kActorAsleep) != 0;
					return r.handle;
				}
			}
			return 0;
		};
		// The order's style: the one its command names (MGSV's call menu labels its attack entry by the dog's equipment and gives that
		// entry's command -- the truth), else the style byte's. A disagreement is logged once per byte.
		const auto byteStyle = plan::StyleOf(g_styleByte.load());
		const auto style = cmd > 0 ? plan::StyleOfCommand(cmd).value_or(byteStyle) : byteStyle;
		if (static int warnedByte = -1; cmd > 0 && style != byteStyle && warnedByte != g_styleByte.load()) {
			warnedByte = g_styleByte.load();
			Log::Info("dog: command {} names {}, the style byte {} reads {} -- the command's is taken", cmd, plan::StyleName(style), warnedByte,
				plan::StyleName(byteStyle));
		}
		bool       pickAsleep = false;
		const auto pickNow = cmd > 0 ? findPick(pickAsleep) : 0;
		// No pick (or an awake one for "Get it") right at the order: the host re-picks for the order's style a frame after its
		// kGEvBuddyOrder, so look again for a few ticks before refusing (review 0.8.0: the table read in the order's own tick still
		// held the pick made for the style the host knew before).
		const bool pickUnfit = !pickNow || (style == proto::kDogFetch && !pickAsleep);
		if (cmd > 0 && !g_job.Busy() && ((a_nowMs - tableMs > 300 && pickRetries < 15) || (pickUnfit && a_nowMs - orderSeenMs < kPickWaitMs))) {
			++pickRetries;
			g_playerAttack.store(cmd);
		} else if (cmd > 0 && !g_job.Busy()) {
			const auto waited = a_nowMs - orderSeenMs;
			pickRetries = 0;
			orderSeenMs = 0;
			++g_jobs;
			g_jobCommand = cmd;
			g_playerCancel.store(0);
			std::uint64_t pick = pickNow;
			if (pick && style == proto::kDogFetch && !pickAsleep) {
				Log::Info("dog: 'Get it' needs a sleeping enemy: {:#x} is awake -- refused", pick);
				pick = 0;
			}
			const auto orderAt = a_nowMs - waited;
			if (!pick && pickSeen && pickSeenMs <= orderAt && pickSeenMs + kPickMemoryMs >= orderAt) {
				// None at the order: the one the host showed the paw over a moment before, while he is still in its table as hostile,
				// alive and no boss (asleep for "Get it"). The host checks him again at the bite.
				if (const auto* r = findRecord(pickSeen); r && (r->flags & proto::kActorHostile) && !(r->flags & (proto::kActorDead | proto::kActorBoss)) &&
					(style != proto::kDogFetch || (r->flags & proto::kActorAsleep))) {
					pick = pickSeen;
					g_pickMemoryUsed.fetch_add(1, std::memory_order_relaxed);
					Log::Info("dog: no pick at the order -- the host's pick of {} ms before it ({:#x}: the paw showed over him) is taken", orderAt - pickSeenMs, pick);
				} else {
					g_pickMemoryMissed.fetch_add(1, std::memory_order_relaxed);
					Log::Info("dog: no pick at the order -- the host's pick of {} ms before it ({:#x}) is no longer in its table as hostile and alive", orderAt - pickSeenMs,
						pickSeen);
				}
			} else if (!pick) {
				Log::Info("dog: no pick at the order, and none in the {} ms before it (last seen {})", kPickMemoryMs,
					pickSeen ? std::format("{:#x} {} ms before", pickSeen, static_cast<std::int64_t>(orderAt) - static_cast<std::int64_t>(pickSeenMs)) : std::string("never"));
			}
			pickSeen = 0;
			const auto step = g_job.Start(pick, style, a_nowMs, g_waitingAtOrder.load());
			Log::Info("dog: order {} ({}) for Elden Ring {:#x}{} ({} ms after the command)", cmd, plan::StyleName(style), pick, pick ? "" : " -- no enemy picked",
				waited);
			if (step.dedicate) {
				proxy::Dedicate(pick, d.targetPuppet);
				g_jobSoldier = 0xFFFF;
			}
			if (step.event) {
				SendEvent(*step.event, style, pick, plan::AttackIdOf(style), step.why);
			}
			if (step.whistle && d.whistleBack) {
				follow::SendCommand(addr::kDogCmdWhistle, 0, "dog: no target");
			}
		} else if (cmd <= 0 && !g_job.Busy()) {
			orderSeenMs = 0;
		}

		if (g_job.Busy()) {
			const auto        ded = proxy::DedicatedState();
			const auto*       rec = findRecord(g_job.Handle());
			plan::JobInput    in;
			in.nowMs = a_nowMs;
			// Gone = absent from 3 good tables in a row (a dead one at once).
			static int absent = 0;
			absent = rec ? 0 : absent + 1;
			in.targetPresent = !(rec && (rec->flags & proto::kActorDead)) && !(rec && !(rec->flags & proto::kActorHostile)) && absent < 3;
			in.proxyReady = ded.handle == g_job.Handle() && ded.ready && ded.soldier != 0xFFFF;
			if (in.proxyReady) {
				g_jobSoldier = ded.soldier;
			}
			{
				std::scoped_lock lock(g_acceptLock);
				in.accepted = g_jobOrderMs && g_lastAccept.ms >= g_jobOrderMs && g_lastAccept.soldier == g_jobSoldier;
			}
			in.fultoned = proxy::Fultoned(g_job.Handle());
			{
				std::scoped_lock lock(g_stateLock);
				std::uint64_t    acceptMs = 0;
				{
					std::scoped_lock al(g_acceptLock);
					acceptMs = g_lastAccept.ms;
				}
				in.contactOver = acceptMs && g_st.exitMs > acceptMs && !g_st.active;
			}
			if (lua.buddyPosValid && ded.soldier != 0xFFFF) {
				in.dogDistance = (lua.buddyPos - ded.pos).LengthXZ();
			}
			const int cancelCommand = g_playerCancel.exchange(0);
			in.cancel = cancelCommand != 0;
			in.cancelCommand = cancelCommand;
			// MGSV's own verdict on the order: its record's target is our soldier from the order on; when it lets go of him without an
			// accept MGSV gave the order up (a fetch: the balloon is fixed, the yank follows ~3 s later; DogLogic waits for it).
			if (g_jobOrderMs && g_jobSoldier != 0xFFFF) {
				const auto target = Get<std::uint16_t>(g_record.load() + addr::kDogOrderTarget);
				if (target && *target == g_jobSoldier) {
					g_orderHeld = true;
					g_orderOffMs = 0;
				} else if (g_orderHeld && target && !g_orderOffMs) {
					g_orderOffMs = a_nowMs;
				}
				in.orderDroppedMs = g_orderHeld && g_orderOffMs ? std::max<std::uint64_t>(1, a_nowMs - g_orderOffMs) : 0;
			}
			const auto step = g_job.Update(in, { .realizeTimeoutMs = d.realizeTimeoutMs, .reachTimeoutMs = d.reachTimeoutMs, .busyMaxMs = d.busyMaxMs });
			const auto handle = g_job.Handle();
			if (cancelCommand && step.event == proto::kDogConnected) {
				g_playerCancel.store(cancelCommand);  // the bite landed in the same tick: the player's command ends it at the next one
			}
			if (step.order) {
				g_jobOrderMs = a_nowMs;
				g_orderHeld = false;
				g_orderOffMs = 0;
				g_watchTarget.store(g_jobSoldier);
				const bool sent = follow::SendCommand(g_jobCommand, g_jobSoldier, "dog: attack order");
				Log::Info("dog: order {} for Elden Ring {:#x} -> soldier {:#x} realized in {} ms -> command {} arg {:#x} {}", g_jobCommand, handle, g_jobSoldier,
					a_nowMs - g_job.StartedMs(), g_jobCommand, g_jobSoldier, sent ? "" : "NOT sent");
			}
			if (step.event) {
				std::uint32_t attack = plan::AttackIdOf(g_job.Style());
				if (*step.event == proto::kDogConnected) {
					std::scoped_lock lock(g_acceptLock);
					// Only an accept since this order names its attack (a fetch connects at the balloon: no accept of its own).
					if (const auto a = g_lastAccept.ms >= g_jobOrderMs ? plan::AttackIdOfMessage(g_lastAccept.message, g_lastAccept.stun) : 0u) {
						attack = a;
					}
				}
				SendEvent(*step.event, g_job.Style(), handle, attack, step.why);
			}
			if (step.release) {
				proxy::Release(step.why);
				g_jobOrderMs = 0;
				g_jobSoldier = 0xFFFF;
			}
			if (step.whistle && d.whistleBack) {
				follow::SendCommand(addr::kDogCmdWhistle, 0, "dog: back to Snake");
			}
			g_engaged.store(g_job.Engaged());
		}
		g_jobBusy.store(g_job.Busy());
		g_jobStageA.store(static_cast<int>(g_job.GetStage()));
		// MGSV's markers of the dog and its target soldier (DogLogic.h IsMgsvMarkerColor): out of the HUD layer while the order runs
		// and a moment after (MGSV fades them out).
		if (g_job.Busy()) {
			g_markersUntil = a_nowMs + 1500;
		}
		capture::hud::SetHideDogMarkers(a_nowMs < g_markersUntil);
	}

	bool KeepBuddyDraw(std::uint64_t a_srcHash, bool a_nearProxy, const float a_origin[3])
	{
		if (!LayerOn() || !Cfg().dog.keepDrawsNearProxy) {
			return false;
		}
		const auto lua = state::Lua();
		if (lua.buddyActive != state::kBuddyTypeDog) {
			return false;
		}
		std::scoped_lock lock(g_meshLock);
		const bool       known = std::find(g_dogMeshes.begin(), g_dogMeshes.end(), a_srcHash) != g_dogMeshes.end();
		// Learned only from draws rooted on the dog's own skin (a skinned part's m_world is its model's root: within ~0.35 m live), with
		// no proxy -- or a just-released one -- within 4 m: a prop or a soldier beside it is never taken for the dog.
		const bool onDog = lua.buddyPosValid && std::hypot(a_origin[0] - lua.buddyPos.x, a_origin[2] - lua.buddyPos.z) < 1.0f;
		if (!a_nearProxy && onDog) {
			if (!known && g_dogMeshes.size() < 256) {
				g_dogMeshes.push_back(a_srcHash);
				g_learned.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}
		if (known) {
			g_keptNearProxy.fetch_add(1, std::memory_order_relaxed);
		}
		return known;
	}

	// ---- console ---------------------------------------------------------------------------------------------------------------
	std::string Command(const std::string& a_args)
	{
		auto& d = MutableCfg().dog;
		if (a_args == "on" || a_args == "off") {
			d.enabled = a_args == "on";
			Log::Info("dog: [Dog] Enabled = {} (console)", d.enabled);
			return std::format("dog: [Dog] Enabled = {} for this session{}", d.enabled, g_sites == 1 ? "" : " (but its sites differ: nothing runs)");
		}
		if (a_args == "treadmill on" || a_args == "treadmill off") {
			d.treadmill = a_args.ends_with("on");
			Log::Info("dog: [Dog] Treadmill = {} (console)", d.treadmill);
			return std::format("dog: [Dog] Treadmill = {}", d.treadmill);
		}
		if (a_args == "attack on" || a_args == "attack off") {
			d.attack = a_args.ends_with("on");
			if (d.attack && g_attackDisabled.exchange(false)) {
				std::string why;
				{
					std::scoped_lock lock(g_textLock);
					why = g_attackDisabledWhy;
				}
				// A fresh alert guard too: its give-up is latched, and without this a later alert MGSV can't calm would never turn
				// Attack off again (review 0.8.0).
				proxy::RearmPhaseGuard("`!dog attack on`");
				Log::Info("dog: Attack allowed again (console; it was off: {})", why);
			}
			Log::Info("dog: [Dog] Attack = {} (console)", d.attack);
			return std::format("dog: [Dog] Attack = {}", d.attack);
		}
		if (a_args == "keep on" || a_args == "keep off") {
			d.keepDrawsNearProxy = a_args.ends_with("on");
			return std::format("dog: [Dog] KeepDrawsNearProxy = {}", d.keepDrawsNearProxy);
		}
		if (a_args == "puppet on" || a_args == "puppet off") {
			d.targetPuppet = a_args.ends_with("on");
			return std::format("dog: [Dog] TargetPuppet = {} (the next order's soldier)", d.targetPuppet);
		}
		if (a_args == "trace on") {
			return StartTrace();
		}
		if (a_args == "trace off") {
			g_tracing.store(false);
			return "dog: trace off";
		}
		if (a_args == "placetrace on" || a_args == "placetrace off") {
			g_traceWant.store(a_args.ends_with("on") ? 1 : -1);
			return std::format("dog: place trace {} at the next frame (PhantomRingDogTrace.csv next to the DLL)", a_args.ends_with("on") ? "on" : "off");
		}
		if (a_args.starts_with("watch ")) {
			unsigned id = 0xFFFF;
			std::sscanf(a_args.c_str() + 6, "%i", reinterpret_cast<int*>(&id));
			g_watchTarget.store(static_cast<std::uint16_t>(id));
			return std::format("dog: trace: messages to {:#x} are logged", id);
		}
		if (a_args == "where") {
			g_whereAsked.store(true);
			std::scoped_lock lock(g_textLock);
			return "dog: where asked for the next frame; last: " + g_where;
		}
		if (a_args == "scan") {
			g_finder.RequestScan();
			return "dog: scan asked";
		}
		if (a_args.starts_with("shift ")) {
			float dx = 0, dz = 0;
			if (std::sscanf(a_args.c_str() + 6, "%f %f", &dx, &dz) != 2 || !std::isfinite(dx) || !std::isfinite(dz) || std::fabs(dx) > 20 || std::fabs(dz) > 20) {
				return "usage: !dog shift <dx> <dz>   (metres, at most 20 each)";
			}
			std::scoped_lock lock(g_reqLock);
			g_testShift = Vec3{ dx, 0.0f, dz };
			return "dog: shift queued for the next frame (see !dog: last place)";
		}
		if (a_args.starts_with("leash ")) {
			float m = 0;
			if (std::sscanf(a_args.c_str() + 6, "%f", &m) != 1 || !(m >= 0.0f) || m > 500.0f) {
				return "usage: !dog leash <m>   (0 = off)";
			}
			d.leash = m;
			return std::format("dog: leash {:.1f} m", m);
		}
		if (a_args == "go" || a_args.starts_with("go ")) {
			// As if the player chose the call menu's attack entry (11-15): the job on the host's kActorDogTarget pick. No number: the
			// entry the dog's equipment gives (its style byte).
			int cmd = plan::CommandOfStyle(plan::StyleOf(g_styleByte.load())).value_or(12);
			if (a_args.size() > 3) {
				std::sscanf(a_args.c_str() + 3, "%d", &cmd);
			}
			if (!plan::IsAttackCommand(cmd)) {
				return "usage: !dog go [11-15]   (11 busy, 12 kill, 13 knock out, 14 wound, 15 fetch; none: the dog's equipment)";
			}
			if (!LayerOn() || !d.attack) {
				return "dog: [Dog] Enabled and Attack must be on (!dog on, !dog attack on)";
			}
			g_playerAttack.store(cmd);
			return std::format("dog: attack command {} taken as the player's (the job runs at the next Lua tick)", cmd);
		}
		if (a_args.starts_with("dedicate ")) {
			// Test: one Elden Ring character gets a soldier of his own (ActorProxy Dedicate), without an order.
			if (a_args == "dedicate off") {
				proxy::Release("console");
				return "dog: dedication released";
			}
			unsigned long long h = 0;
			if (std::sscanf(a_args.c_str() + 9, "%lli", &h) != 1 || !h) {
				return "usage: !dog dedicate <ActorRecord handle 0x...>|off";
			}
			proxy::Dedicate(h, d.targetPuppet);
			return std::format("dog: Elden Ring {:#x} dedicated ({})", h, d.targetPuppet ? "puppet" : "restrict-notice only");
		}
		if (a_args.starts_with("order ") || a_args.starts_with("player ")) {
			const bool player = a_args.starts_with("player ");
			int        cmd = 0;
			unsigned   arg = 0;
			if (std::sscanf(a_args.c_str() + (player ? 7 : 6), "%d %i", &cmd, reinterpret_cast<int*>(&arg)) < 1 || !plan::IsDogCommand(cmd) || arg > 0xFFFF) {
				return "usage: !dog order|player <8-16> [arg]   (8 whistle, 9 wait, 10 bark, 11-15 attack/fetch on GameObject id arg, 16 DD; player: as "
				       "the call menu gives it)";
			}
			std::scoped_lock lock(g_orderLock);
			g_consoleOrders.push_back({ cmd, static_cast<std::uint16_t>(arg), player });
			return std::format("dog: buddy command {} arg {:#x}{} queued for MGSV's Lua thread", cmd, arg, player ? " (as the player's)" : "");
		}
		if (a_args.starts_with("style ")) {
			int s = -1;
			if (std::sscanf(a_args.c_str() + 6, "%d", &s) != 1 || s < 0 || s > 7) {
				return "usage: !dog style <0-7>   (test: the dog system's raw style byte; 0 busy, 1 fetch, 2 knock out, 3 wound, 4 kill, 5-7 bite)";
			}
			std::scoped_lock lock(g_orderLock);
			g_styleWrite = s;
			return std::format("dog: style byte {} queued (test only; MGSV may set it again from the equipment)", s);
		}
		return Describe();
	}

	namespace
	{
		void Snapshot()
		{
			const auto sys = g_sys.load();
			const auto rec = g_record.load();
			const auto bits = Get<std::uint32_t>(rec + addr::kDogOrderBits);
			const auto timer = Get<float>(rec + addr::kDogOrderTimer);
			const auto target = Get<std::uint16_t>(rec + addr::kDogOrderTarget);
			const auto can = sys ? CanOrder() : std::nullopt;
			const auto& d = Cfg().dog;
			std::string rows;
			for (std::uint32_t atk = 144; atk <= 147; ++atk) {
				std::uint16_t lethal = 0, nonLethal = 0;
				std::uint32_t share = 0;
				rows += combat::AttackRow(atk, lethal, nonLethal, share) ? std::format(" {}={}/{} ({})", atk, lethal, nonLethal, share) : std::format(" {}=?", atk);
			}
			StateSeen st;
			{
				std::scoped_lock lock(g_stateLock);
				st = g_st;
			}
			Accept acc;
			{
				std::scoped_lock lock(g_acceptLock);
				acc = g_lastAccept;
			}
			const auto  ded = proxy::DedicatedState();
			std::string jobLast, disabledWhy;
			{
				std::scoped_lock lock(g_textLock);
				jobLast = g_jobLast;
				disabledWhy = g_attackDisabledWhy;
			}
			std::string text = std::format(
				"dog: sites {} layer {} (treadmill {}, attack {}{}, puppet {}, keep draws {}, whistle back {}); system {:#x} index {} style byte {} ({}) "
				"vars equip {} mode {} flag {}; can take orders {}; order record {:#x} bits {} timer {} target {}; waiting {}; messages {} ({} orders), "
				"player commands {}; the host's D-Dog layer {} ({} attack orders left to MGSV)\n"
				"  location object {:#x} {} ({} scans), feet {}, {} moved with jumps ({} failed, {} undone by MGSV and given again), {} leashes (leash {:.0f} m, max {:.0f} m); "
				"last place: {}\n"
				"  attack state: {} target {:#x} mode {} result {} flags {:#x}, {} entries; accepts {} (last soldier {:#x} {:#x}{}), refusals {}\n"
				"  job: {} on {:#x} (soldier {:#x}), {} jobs, events ordered {} connected {} released {} refused {} lost {}; engaged {}; dedicated {:#x} soldier "
				"{:#x}{}; last: {}\n"
				"  ATK rows (lethal/non-lethal (share)):{}; capture: {} dog meshes learned, {} draws kept near a proxy\n"
				"  the host's pick before an order: {} reads, {} orders took the one it had a moment before ({} not: gone by then)\n"
				"  shown to Elden Ring: {} frames held to the dog's speed (most {:.2f} m behind), {} MGSV warps taken at once; staged with Snake: {} taken "
				"along, {} back, {} not moved; its soldier {}; Wait kept in Elden Ring's terms: {} waits watched, shown {:.2f},{:.2f} m on (most {:.2f} m; slip "
				"now {:.2f},{:.2f})",
				g_sites, LayerOn() ? "on" : "off", d.treadmill, d.attack, g_attackDisabled.load() ? std::format(" -- OFF this session: {}", disabledWhy) : "",
				d.targetPuppet, d.keepDrawsNearProxy, d.whistleBack, sys, g_index.load(), g_styleByte.load(), plan::StyleName(plan::StyleOf(g_styleByte.load())),
				VarU8(addr::kVarBuddyDogEquipType).value_or(255), VarU8(addr::kVarBuddyDogMode).value_or(255), VarU8(addr::kVarBuddyDogFlag).value_or(255),
				can ? (*can ? "yes" : "no") : "?", rec, bits ? std::format("{:#x}", *bits) : std::string("?"), timer ? std::format("{:.1f}", *timer) : std::string("?"),
				target ? std::format("{:#x}", *target) : std::string("?"), g_waiting.load(), g_messages.load(), g_orderMessages.load(), g_playerDogCommands.load(),
				g_hostLayer.load() < 0 ? "not read yet" : g_hostLayer.load() ? "on" : "OFF", g_passedToMgsv.load(), g_finder.Candidate(), g_finder.Confirmed() ? "confirmed" : g_finder.Candidate() ? "candidate" : "none", g_finder.Scans(),
				g_feet ? std::format("{:.2f},{:.2f},{:.2f}", g_feet->x, g_feet->y, g_feet->z) : std::string("-"), g_shifts, g_shiftFails, g_shiftsRedone, g_leashes, d.leash,
				d.treadmillMaxDistance, g_lastPlace, st.active ? "ACTIVE" : "idle", st.target, st.mode, st.result, st.flags, st.enters, g_accepts.load(), acc.soldier,
				acc.message, acc.stun ? " stun" : "", g_refusals.load(), plan::StageName(g_job.GetStage()), g_job.Handle(), g_jobSoldier, g_jobs, g_phaseCounts[0],
				g_phaseCounts[1], g_phaseCounts[2], g_phaseCounts[3], g_phaseCounts[4], g_engaged.load(), ded.handle, ded.soldier, ded.ready ? " (ready)" : "", jobLast,
				rows, g_learned.load(), g_keptNearProxy.load(), g_pickReads.load(), g_pickMemoryUsed.load(), g_pickMemoryMissed.load(), g_limiter.Limited(), g_limiter.MaxLag(), g_limiter.Teleports(), g_stagedAlong, g_stagedBack,
				g_stagedFails, g_heldNow ? "HELD by the bite" : "not held", g_waitsWatched, g_waitOffset.x, g_waitOffset.z, g_waitOffsetMax, proxy::Slip().x,
				proxy::Slip().z);
			std::scoped_lock lock(g_describeLock);
			g_describe = std::move(text);
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_describeLock);
		return g_describe;
	}

	// Called from ActorProxyGame's phase lock (Lua thread) when the alert phase couldn't be held while the dog's soldier was out.
	void DisableAttack(const std::string& a_why)
	{
		// The text first, then the flag: a reader that sees the flag set finds the reason (both under the text lock).
		std::scoped_lock lock(g_textLock);
		if (!g_attackDisabled.load()) {
			g_attackDisabledWhy = a_why;
			g_attackDisabled.store(true);
			Log::Warn("dog: Attack OFF for this session -- {} (`!dog attack on` tries again)", a_why);
		}
	}
}
