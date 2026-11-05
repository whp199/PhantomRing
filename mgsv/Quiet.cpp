// Quiet as a sniper buddy in Elden Ring: the game glue (Quiet.h; the rules in QuietLogic.h; docs/research/quiet.md).
#include "Quiet.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "BuddyLocation.h"
#include "BuddyLogic.h"
#include "Combat.h"
#include "Config.h"
#include "Follow.h"
#include "Hold.h"
#include "Hook.h"
#include "Hud.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "QuietLogic.h"
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
#include <deque>
#include <format>
#include <mutex>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace phantom::guest::quiet
{
	namespace
	{
		using logic::Vec3;
		using HandlerFn = std::int32_t* (*)(std::uint8_t* a_sys, std::int32_t* a_result, std::uint32_t a_index, std::uint64_t* a_message);
		using HitFn = void (*)(void* a_component, std::uint32_t a_index, void* a_box, std::uint16_t a_target, std::uint16_t a_attack);
		using CantFn = void (*)(void* a_component, std::uint32_t a_index, void* a_table);
		using AccessorFn = std::uintptr_t (*)();

		constexpr std::uint16_t kQuietType = 0x12;  // GameObject type 18 (BUDDYQUIET2): ids 0x2400 | index
		// Her messages (§2.4; the buddy command function's cases and the block controller's).
		constexpr std::uint64_t kMsgFireAim = 0x7E380C754E3A, kMsgCover = 0x56A36C25640E, kMsgShootThis = 0xE26B1E1A8A9E, kMsgTalk = 0xA265A20DFEEF,
		                        kMsgMoveTo = 0xF27142902A3F, kMsgLeave = buddy::plan::kMsgLeave, kMsgEspionage = 0x51636A12C953, kMsgHit = 0x5CFEE2E03274;

		int       g_sites{ -1 };  // -1 not checked, 0 differ (the layer stays off), 1 ok
		bool      g_hitOk{ false }, g_cantOk{ false }, g_varsOk{ false }, g_staticOk{ false }, g_recordOk{ false }, g_commandsOk{ false }, g_menuOk{ false };
		HandlerFn g_handler{ nullptr };
		HitFn     g_hit{ nullptr };
		CantFn    g_cant{ nullptr };

		// ---- what the detours learn ---------------------------------------------------------------------------------------------
		std::atomic<std::uintptr_t> g_sys{ 0 };
		std::atomic<std::uint32_t>  g_index{ 0 };
		std::atomic<std::uint64_t>  g_messages{ 0 }, g_orderMessages{ 0 };
		std::mutex                  g_seenLock;
		std::vector<std::uint64_t>  g_seenIds;

		struct HitSeen
		{
			std::uint16_t target{ 0xFFFF };
			std::uint16_t attack{ 0 };
			std::uint32_t index{ 0 };
			std::uint8_t  flag60{ 0 };  // R[0] bit 60 (the message's +0xC byte)
			std::uint64_t ms{ 0 };
		};
		std::mutex              g_hitLock;
		HitSeen                 g_lastHit;
		std::deque<HitSeen>     g_hitLog;
		std::atomic<std::uint64_t> g_hits{ 0 }, g_cants{ 0 }, g_cantMs{ 0 };

		// ---- her place (game thread) ----------------------------------------------------------------------------------------------
		buddyloc::Finder    g_finder{ "quiet" };
		std::optional<Vec3> g_feet;
		std::optional<Vec3> g_pendingShift;
		std::string         g_pendingWhy;
		std::uint64_t       g_shifts{ 0 }, g_shiftFails{ 0 }, g_places{ 0 }, g_placeFails{ 0 };
		std::uint16_t       g_quietId{ 0xFFFF };
		proxy::plan::OffsetHistory g_k;
		Vec3                g_kNow;
		bool                g_scanWanted{ false };  // `!quiet scan` (the finder runs while Quiet is out and this or a holder wants it)
		std::string         g_lastPlace{ "none" };  // game thread; the console reads g_snap's copy (review: no unlocked string read)
		std::vector<float>  g_region;               // [Quiet] MirrorRegion as the game thread uses it (`!quiet set region` -> g_regionNew)
		bool                g_regionInit{ false };

		// The holder: where her MGSV body is kept (a point in odometer space, so the treadmill doesn't move it), why, and how close.
		enum class HoldWhy : std::uint8_t { kNone, kConsole, kPost, kSeat };
		struct Holder
		{
			HoldWhy       why{ HoldWhy::kNone };
			Vec3          odo;            // the point - k (odometer space)
			float         yaw{ 0.0f };
			float         tolerance{ 3.0f };
			std::uint32_t seq{ 0 };       // the post's / the rest's sequence
			int           tries{ 0 };
			bool          at{ false };    // she stands within tolerance of it
		};
		std::mutex          g_holdLock;  // the console's request vs the game thread
		std::optional<Holder> g_consoleHold;
		bool                g_consoleHoldOff{ false };
		std::optional<Vec3> g_testShift;
		std::optional<std::vector<float>> g_regionNew;  // `!quiet set region` (console -> game thread)
		Holder              g_hold;  // game thread
		// What the console shows of the game thread's state: a copy Update publishes under g_holdLock (review: Describe read g_feet,
		// g_hold and g_lastPlace unlocked while Update wrote them).
		struct Snapshot
		{
			std::optional<Vec3> feet;
			HoldWhy             holdWhy{ HoldWhy::kNone };
			Vec3                holdAt;
			bool                holdThere{ false };
			std::string         lastPlace{ "none" };
			std::uintptr_t      candidate{ 0 };
			bool                confirmed{ false };
			std::uint64_t       scans{ 0 };
		};
		Snapshot            g_snap;  // under g_holdLock
		std::atomic<std::uint32_t> g_postEcho{ 0 }, g_graceEcho{ 0 };
		std::atomic<bool>   g_atPost{ false };
		std::atomic<bool>   g_away{ false };

		// The host's post and grace (game thread, from HostState each frame).
		struct HostView
		{
			bool          valid{ false };
			bool          layer{ false };
			bool          atGrace{ false };
			std::uint32_t postSeq{ 0 }, graceSeq{ 0 };
			Vec3          postOdo;  // in odometer space
			bool          postValid{ false };
			float         postYaw{ 0.0f };
			Vec3          seatOdo, centreOdo;
			bool          seatValid{ false }, centreValid{ false };
			float         seatYaw{ 0.0f };
		};
		HostView                 g_host;
		std::atomic<int>         g_hostLayer{ -1 };
		std::atomic<bool>        g_atGrace{ false };
		plan::SitHold            g_sit;  // game thread
		// Her own move orders for the sit (game thread -> Lua thread): the seat (aim off: the rifle down) or getting up (aim on).
		struct SitMove
		{
			std::uint32_t id{ 0 };
			Vec3          to;
			bool          aim{ false };
			bool          getUp{ false };
		};
		std::mutex                 g_sitLock;
		std::optional<SitMove>     g_sitMove;
		std::atomic<std::uint32_t> g_sitMoveSent{ 0 };
		std::atomic<std::uint64_t> g_sitPlaced{ 0 };
		// Getting up (game thread). Her get-up order (MoveToPosition with her aim) makes MGSV's Quiet relocate as she does on her own -- a
		// turn, then a cloaked dash drawn by nobody, her location owned by her AI meanwhile -- so sent the moment Snake left the grace she
		// vanished in view (live 0.9.0 integration, Rampart Tower), and sent with her next post she ran back to where the order found her
		// and the holder's placements didn't hold (Gatefront: 40 in a burst). So she keeps her rest (crouched, rifle upright: "on watch")
		// and is placed at her posts as she is, and her order goes out only while the host has no post for her (on the move: unseen),
		// where her own relocation is what Elden Ring would show anyway.
		std::uint64_t              g_sitOrderMs{ 0 };
		bool                       g_upPending{ false };
		std::uint64_t              g_upSince{ 0 }, g_upWaitSince{ 0 };
		std::uint32_t              g_upWait{ 0 }, g_upIds{ 0x80000000u };
		// The treadmill: the host's post comes back in MGSV's world as of the GuestState it consumed (corrected by k 33 ms ago), so for a
		// frame or two after a jump it can be off by the jump (live: placed 5.9 m one way and 6.0 m back the next frame). The holder
		// waits kKSettleMs after k moved before it re-places her.
		Vec3                       g_kLast;
		std::uint64_t              g_kMovedMs{ 0 };
		constexpr std::uint64_t    kKSettleMs = 250;

		// Cloak (MGSV draws her not at all while she dashes): her speed.
		std::optional<Vec3>        g_lastSeen;
		std::atomic<std::uint64_t> g_cloakUntil{ 0 };
		// Capture's view of her (any thread): her root.
		std::atomic<float>         g_rootX{ NAN }, g_rootY{ NAN }, g_rootZ{ NAN };
		std::atomic<float>         g_snakeX{ NAN }, g_snakeZ{ NAN };  // Snake's MGSV root (the rigid keys' bootstrap)
		std::atomic<std::uint64_t> g_kept{ 0 }, g_propsDropped{ 0 }, g_rigidConfirmed{ 0 };
		// Her rigid draws (her rifle) by mesh key (quiet.md §3.8, review): a key seen within RigidRadius of her at two places at least
		// kKeyMoved apart moved with her -- hers; a static prop never does. Until then kept only while she is within kKeyNearSnake of
		// Snake (deployed beside him on the apron, where nothing else stands).
		struct RigidKey
		{
			std::uint64_t key{ 0 };
			float         x{ 0 }, z{ 0 };  // her root where it was first seen near her
			bool          confirmed{ false };
		};
		constexpr float       kKeyMoved = 5.0f, kKeyNearSnake = 15.0f;
		std::mutex            g_keyLock;
		std::vector<RigidKey> g_rigidKeys;
		std::atomic<float>         g_skinnedRadius{ 1.5f };
		// Heading (skin root, fresh)
		std::atomic<float>         g_skinYaw{ NAN };

		// ---- switches for the session (console) -----------------------------------------------------------------------------------
		std::atomic<int>  g_layerForce{ -1 };   // -1 the ini's [Quiet] Enabled, 0 off, 1 on
		std::atomic<int>  g_keepMarksForce{ -1 };
		std::atomic<bool> g_ordersOff{ false };  // the phase lock gave up (DisableOrders)
		std::string       g_ordersOffWhy;
		std::atomic<bool> g_borrowUnguardedTest{ false };
		std::atomic<bool> g_strikeAgain{ false };  // a load re-enables MGSV's menu items: strike them again  // `!quiet borrow test-unguarded`: only the borrow's own guard check fails

		// ---- the borrow (Lua thread) --------------------------------------------------------------------------------------------
		struct Borrow
		{
			bool                                      active{ false };
			plan::SaveBits                            snapBits;
			std::array<std::uint8_t, plan::kBlockBytes> snap{};
			std::uint8_t                              staticSnap{ 0 };
			plan::SaveState                           state{ plan::SaveState::kUnknown };
			std::uint64_t                             atMs{ 0 };
			int                                       reapplied{ 0 };
		};
		std::mutex      g_borrowLock;
		Borrow          g_borrow;
		plan::SaveBits  g_bits;  // the last read-out
		std::string     g_borrowText{ "not checked" };
		std::string     g_lastDiff{ "none" };
		std::atomic<int> g_borrowAsk{ 0 };  // 1 borrow, 2 put back, 3 bits, 4 diff now (console -> Lua thread)
		bool            g_bootLogged{ false };
		// The ini's borrow (Borrow + BorrowListInIdroid / [Buddy] Type = quiet) waits for the host's Quiet layer: without it she would be
		// 0.8.0's Quiet with none of the layer's safeguards (review: every save would list an unguarded Quiet). Also set after the put-back
		// for "Elden Ring is gone", so she is borrowed again when it is back (review: she stayed un-borrowed until the next load).
		bool            g_borrowPending{ false };
		bool            g_borrowPendingLogged{ false };
		bool            g_borrowByHand{ false };  // `!quiet borrow off`: no borrow by itself until `!quiet borrow` or the next load
		std::uint64_t   g_borrowTriedMs{ 0 };
		std::uint64_t   g_enableAt{ 0 };  // the Enable* states re-sent every kEnableEveryMs (idempotent; a lost send heals)
		constexpr std::uint64_t kEnableEveryMs = 5000;

		// Lua work queued by the console (any thread -> Lua thread).
		std::mutex               g_luaLock;
		std::vector<std::string> g_luaQueue;
		std::string              g_luaLast;

		// ---- the order job (Lua thread) -------------------------------------------------------------------------------------------
		plan::OrderJob             g_job;
		std::atomic<int>           g_playerOrder{ -1 };
		std::atomic<bool>          g_jobBusy{ false }, g_engaged{ false };
		std::uint16_t              g_jobSoldier{ 0xFFFF };
		std::uint64_t              g_pickSeen{ 0 }, g_pickSeenMs{ 0 }, g_lastBinocMs{ 0 };
		std::array<std::uint32_t, 5> g_phaseCounts{};
		std::string                g_jobLast{ "none" };
		std::mutex                 g_textLock;
		std::atomic<bool>          g_standIn{ false };     // a stand-in is dedicated for her (job or MarkProxy)
		std::atomic<bool>          g_marked{ false };      // kBuddyQuietMarked
		std::atomic<std::uint64_t> g_markersUntil{ 0 };
		std::uint64_t              g_standInHandle{ 0 };
		std::atomic<bool>          g_standInConsole{ false };  // `!quiet standin`: kept until `!quiet standin off` (probes)
		std::atomic<int>           g_menuFire{ -1 };           // `!quiet menufire on|off`: the call menu's Fire / Take aim made live (-1 = [Quiet] Orders')
		std::atomic<std::uint64_t> g_menuForced{ 0 };
		int                        g_fireSent{ -1 }, g_aimSent{ -1 }, g_dyingSent{ -1 };  // the Enable* states sent to this Quiet (-1 none)
		std::uint16_t              g_enableFor{ 0xFFFF };  // ...to this object id (a new Quiet starts over)
		std::atomic<int>           g_weapon{ proto::kQuietWeaponUnknown };
		std::atomic<int>           g_equip{ -1 };
		std::atomic<int>           g_liveWeapon{ -1 };
		std::atomic<bool>          g_cover{ false };
		std::atomic<std::uint64_t> g_passed{ 0 }, g_refused{ 0 }, g_taken{ 0 };

		// ---- the trace (game thread) -----------------------------------------------------------------------------------------------
		FILE*            g_traceFile{ nullptr };
		std::uint64_t    g_traceRows{ 0 };
		std::atomic<int> g_traceWant{ 0 };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return a_at ? SafeRead<T>(reinterpret_cast<const void*>(a_at)) : std::nullopt;
		}

		bool Put(std::uintptr_t a_at, const void* a_in, std::size_t a_n)
		{
			SIZE_T put = 0;
			return a_at && ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(a_at), a_in, a_n, &put) && put == a_n;
		}

		std::string V3(const std::optional<Vec3>& a_v)
		{
			return a_v ? std::format("{:.2f},{:.2f},{:.2f}", a_v->x, a_v->y, a_v->z) : std::string("-");
		}

		bool LayerOn()
		{
			const int f = g_layerForce.load();
			return g_sites == 1 && (f < 0 ? Cfg().quiet.enabled : f == 1);
		}

		bool QuietOut(const state::LuaView& a_lua)
		{
			return a_lua.InGame() && a_lua.buddyActive == state::kBuddyTypeQuiet && a_lua.buddyId != 0xFFFF && (a_lua.buddyId >> 9) == kQuietType &&
			       !state::BuddyMissing();
		}

		// The host's Quiet layer (HostState kHostQuietLayer) as last read (game thread keeps it).
		bool HostLayer() { return g_hostLayer.load() == 1; }

		std::uintptr_t Record()
		{
			if (!g_recordOk) {
				return 0;
			}
			return plan::RecordAddress([](std::uintptr_t a) { return Get<std::uint64_t>(a); }, g_sys.load(), g_index.load());
		}

		std::optional<std::uint16_t> VarU16(std::uint32_t a_offset)
		{
			const auto block = Get<std::uintptr_t>(addr::kVarsPtr);
			return block && *block ? Get<std::uint16_t>(*block + a_offset) : std::nullopt;
		}

		const char* MessageName(std::uint64_t a_id)
		{
			switch (a_id) {
			case kMsgFireAim:
				return "fire/aim";
			case kMsgCover:
				return "cover";
			case kMsgShootThis:
				return "shoot this";
			case kMsgTalk:
				return "talk";
			case kMsgMoveTo:
				return "move to a point";
			case kMsgLeave:
				return "leave";
			case kMsgEspionage:
				return "recon";
			default:
				return nullptr;
			}
		}

		// ---- the detours (observe only) -----------------------------------------------------------------------------------------
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
			if (id == kMsgLeave) {
				// MGSV's buddy system sends her away (the iDroid's DISMISS QUIET / CHANGE EQUIPMENT / another buddy; BuddyLogic.h)
				state::NoteBuddyLeave(state::kBuddyTypeQuiet, ::GetTickCount64());
			}
			if (g_sys.exchange(reinterpret_cast<std::uintptr_t>(a_sys)) != reinterpret_cast<std::uintptr_t>(a_sys)) {
				Log::Info("quiet: the Quiet system is {:#x} (her index {}, {} Quiets)", reinterpret_cast<std::uintptr_t>(a_sys), a_index, *count);
			}
			g_index.store(a_index, std::memory_order_relaxed);
			const char* name = MessageName(id);
			{
				std::scoped_lock lock(g_seenLock);
				if (std::find(g_seenIds.begin(), g_seenIds.end(), id) == g_seenIds.end() && g_seenIds.size() < 64) {
					g_seenIds.push_back(id);
					Log::Info("quiet: her handler got message {:#x}{} (arg {:#x}) -- first of its kind", id, name ? std::format(" ({})", name) : std::string(), arg);
				}
			}
			if (id == kMsgFireAim || id == kMsgCover || id == kMsgShootThis || id == kMsgTalk || id == kMsgMoveTo) {
				g_orderMessages.fetch_add(1, std::memory_order_relaxed);
				const auto rec = Record();
				const auto act = Get<std::uint16_t>(rec + plan::kRecTargetActive), req = Get<std::uint16_t>(rec + plan::kRecTargetRequested),
				           aim = Get<std::uint16_t>(rec + plan::kRecTargetAim);
				const auto dis = Get<std::uint32_t>(rec + plan::kRecDisabled), ordA = Get<std::uint32_t>(rec + plan::kRecActive),
				           ordR = Get<std::uint32_t>(rec + plan::kRecRequested);
				const auto f = [](const auto& o) { return o ? std::format("{:#x}", *o) : std::string("?"); };
				Log::Info("quiet: order {} (arg {:#x}) taken by her handler: R {:#x} targets active {} requested {} aim {}, orders {} requested {}, disabled {}",
					name ? name : "?", arg, rec, f(act), f(req), f(aim), f(ordA), f(ordR), f(dis));
			}
			return r;
		}

		void HitDetour(void* a_component, std::uint32_t a_index, void* a_box, std::uint16_t a_target, std::uint16_t a_attack)
		{
			g_hit(a_component, a_index, a_box, a_target, a_attack);
			if (a_target == 0xFFFF) {
				return;
			}
			HitSeen h;
			h.target = a_target;
			h.attack = a_attack;
			h.index = a_index;
			h.ms = ::GetTickCount64();
			// R[0] bit 60 as the sender reads it (the message's +0xC byte), through her component's system.
			if (const auto sys = Get<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(a_component) + 0x70)) {
				const auto rec = plan::RecordAddress([](std::uintptr_t a) { return Get<std::uint64_t>(a); }, *sys, a_index);
				if (const auto r0 = Get<std::uint64_t>(rec)) {
					h.flag60 = static_cast<std::uint8_t>((*r0 >> 60) & 1);
				}
				if (*sys && g_sys.exchange(*sys) != *sys) {
					Log::Info("quiet: the Quiet system is {:#x} (learned at her hit)", *sys);
				}
			}
			const auto n = g_hits.fetch_add(1, std::memory_order_relaxed);
			{
				std::scoped_lock lock(g_hitLock);
				g_lastHit = h;
				g_hitLog.push_back(h);
				if (g_hitLog.size() > 16) {
					g_hitLog.pop_front();
				}
			}
			if (n < 500) {
				Log::Info("quiet: her hit: attack {} ({}) on {:#x}{} (her index {}, R[0] bit 60 = {})", a_attack, plan::WeaponName(plan::WeaponOfAttack(a_attack)),
					a_target, (a_target & 0xFE00) == 0x400 ? " (a soldier)" : " (not a soldier: no message sent)", a_index, h.flag60);
			}
		}

		void CantDetour(void* a_component, std::uint32_t a_index, void* a_table)
		{
			g_cant(a_component, a_index, a_table);
			g_cantMs.store(::GetTickCount64(), std::memory_order_relaxed);
			if (g_cants.fetch_add(1, std::memory_order_relaxed) < 200) {
				Log::Info("quiet: her \"can't\" path ran (no target for the order; her index {})", a_index);
			}
		}

		// ---- events ----------------------------------------------------------------------------------------------------------------
		// MGSV's share of a soldier's life (1/10000), lethal or non-lethal by the row (Guilty's 291 is 0 / 1200: its non-lethal share).
		std::uint32_t ShareOf(std::uint32_t a_attack)
		{
			std::uint16_t lethal = 0, nonLethal = 0;
			std::uint32_t share = 0;
			if (!a_attack || !combat::AttackRow(a_attack, lethal, nonLethal, share)) {
				return 0;
			}
			return share ? share : static_cast<std::uint32_t>(nonLethal) * 10000u / 2600u;
		}

		proto::QuietWeapon Weapon() { return static_cast<proto::QuietWeapon>(g_weapon.load()); }

		void SendShot(proto::QuietPhase a_phase, std::uint64_t a_handle, std::uint32_t a_attack, std::uint32_t a_b, const char* a_why)
		{
			proto::GuestEvent e{};
			e.type = proto::kGEvQuietShot;
			// The attack id's rifle; an id outside 290-292 (a suppressed variant's, unverified) takes the equipment var's (review: an unknown
			// rifle must never reach the host as "unknown", which fell through to lethal there).
			const auto weapon = plan::ShotWeapon(a_attack, Weapon());
			e.a = proto::QuietShotA(a_phase, weapon, a_attack);
			e.b = a_b;
			const auto fr = state::Frame();
			e.frame = static_cast<std::uint32_t>(fr.frame);
			const auto lua = state::Lua();
			const Vec3 at = lua.buddyPosValid ? lua.buddyPos : fr.root;
			e.v0[0] = at.x;
			e.v0[1] = at.y;
			e.v0[2] = at.z;
			const auto ded = proxy::DedicatedState();
			if (a_handle && ded.handle == a_handle && ded.soldier != 0xFFFF) {
				const Vec3  d = ded.pos - at;
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
				auto             last = std::format("{} on {:#x} ({})", plan::PhaseName(a_phase), a_handle, a_why);
				std::scoped_lock lock(g_textLock);
				g_jobLast = std::move(last);
			}
			Log::Info("quiet: kGEvQuietShot {} ({}, MGSV attack {}, b {}) on Elden Ring {:#x}: {}{}", plan::PhaseName(a_phase), plan::WeaponName(weapon), a_attack, a_b,
				a_handle, a_why, sent ? "" : " -- event ring full, NOT sent");
		}

		// ---- the borrow (Lua thread) ------------------------------------------------------------------------------------------------
		constexpr const char* kStateLua =
			"local Q=BuddyType.QUIET local S=TppBuddyService local F=BuddyCommonFlag "
			"local function b(f, ...) local ok, v = pcall(f, ...) if ok and v then return 1 end return 0 end "
			"local function n(f, ...) local ok, v = pcall(f, ...) if ok and type(v)=='number' then return v end return -1 end "
			"local gone = 0 if gvars and gvars.str_didLostQuiet then gone = 1 end "
			"local calls = -1 pcall(function() calls = vars.buddyCallCount[Q] end) "
			"return string.format('obt=%d sortie=%d lost=%d dead=%d hosp=%d dying=%d kill=%d dis=%d gone=%d bond=%d calls=%d', "
			"b(S.DidObtainBuddyType,Q), b(S.CanSortieBuddyType,Q), b(S.CheckBuddyCommonFlag,F.BUDDY_QUIET_LOST), b(S.IsDeadBuddyType,Q), "
			"b(S.CheckBuddyCommonFlag,F.BUDDY_QUIET_HOSPITALIZE), b(S.CheckBuddyCommonFlag,F.BUDDY_QUIET_DYING), b(S.CheckBuddyCommonFlag,F.BOSS_QUIET_KILL), "
			"b(S.IsDisableBuddyType,Q), gone, n(S.GetFriendlyPoint,BuddyFriendlyType.QUIET), calls)";

		plan::SaveBits ReadBits(const LuaCall& a_call)
		{
			const auto text = a_call(kStateLua);
			auto       b = plan::ParseSaveBits(text);
			if (!b.known) {
				Log::Warn("quiet: her save state couldn't be read ({})", text);
			}
			return b;
		}

		bool ReadBlock(std::array<std::uint8_t, plan::kBlockBytes>& a_out, std::uint8_t& a_static)
		{
			const auto block = Get<std::uintptr_t>(addr::kVarsPtr);
			if (!block || !*block) {
				return false;
			}
			SIZE_T got = 0;
			if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(*block + plan::kBlockFirst), a_out.data(), a_out.size(), &got) ||
				got != a_out.size()) {
				return false;
			}
			const auto st = g_staticOk ? Get<std::uint8_t>(addr::kQuietDisableStatic) : std::nullopt;
			a_static = st.value_or(0);
			return true;
		}

		std::uint64_t Fnv(const std::uint8_t* a_p, std::size_t a_n)
		{
			std::uint64_t h = 1469598103934665603ull;
			for (std::size_t i = 0; i < a_n; ++i) {
				h = (h ^ a_p[i]) * 1099511628211ull;
			}
			return h;
		}

		plan::BorrowMode Mode()
		{
			switch (Cfg().quiet.borrow) {
			case QuietBorrow::kMemory:
				return plan::BorrowMode::kMemory;
			case QuietBorrow::kAlways:
				return plan::BorrowMode::kAlways;
			default:
				return plan::BorrowMode::kOff;
			}
		}

		// The borrow: snapshot, the plan's calls, logged. a_mode kOff = refused. Lua thread.
		std::string DoBorrow(bool a_guarded, plan::BorrowMode a_mode, const LuaCall& a_call, const char* a_why)
		{
			if (g_sites != 1 || !g_varsOk) {
				return "quiet: borrow refused -- her sites differ in this exe";
			}
			const bool guarded = a_guarded && !g_borrowUnguardedTest.load();
			if (!guarded) {
				return g_borrowUnguardedTest.load() ? "quiet: borrow refused -- the save guards are incomplete (test: the borrow's own guard check made to fail)"
				                                    : "quiet: borrow refused -- the save guards are incomplete: it could be saved";
			}
			const auto bits = ReadBits(a_call);
			g_bits = bits;
			const auto state = plan::Classify(bits);
			std::scoped_lock lock(g_borrowLock);
			if (!g_borrow.active) {
				Borrow b;
				if (!ReadBlock(b.snap, b.staticSnap)) {
					return "quiet: borrow refused -- MGSV's vars block can't be read";
				}
				b.snapBits = bits;
				b.state = state;
				b.atMs = ::GetTickCount64();
				const auto plan = plan::BorrowPlan(bits, a_mode);
				if (plan.empty()) {
					return std::format("quiet: save state {} ({}) -- nothing to borrow under Borrow = {}", plan::StateName(state), plan::DescribeBits(bits),
						a_mode == plan::BorrowMode::kOff ? "off" : a_mode == plan::BorrowMode::kMemory ? "memory" : "always");
				}
				// Each save state is borrowed only once it passed its live round trip (quiet.md Q14, 2026-11-04: gone, never recruited, killed,
				// recruited-not-deployable). Sickbay / dying / dead weren't checked live: not borrowed.
				if (plan::Sick(bits)) {
					return std::format("quiet: borrow for {} off: a Quiet in sickbay / dying / dead isn't borrowed (not checked live, quiet.md Q14) ({})",
						plan::StateName(state), plan::DescribeBits(bits));
				}
				std::string results;
				for (const auto& stmt : plan) {
					const auto r = a_call(std::format("local ok, e = pcall(function() {} end) return tostring(ok) .. (e and (' ' .. tostring(e)) or '')", stmt));
					results += std::format("{}{} -> {}", results.empty() ? "" : "; ", stmt.substr(stmt.find('.') + 1), r);
				}
				b.active = true;
				g_borrow = b;
				const auto after = ReadBits(a_call);
				g_bits = after;
				Log::Info("quiet: borrowed for this session (memory only -- MGSV's save is not changed): save state {} ({}); snapshot {:#x} + static {:#04x}; {}; "
				          "now {} ({})",
					plan::StateName(state), plan::DescribeBits(bits), Fnv(b.snap.data(), b.snap.size()), b.staticSnap, results, plan::DescribeBits(after), a_why);
				return std::format("quiet: borrowed ({}): {} -> {}", plan::StateName(state), results, plan::DescribeBits(after));
			}
			// Already borrowed: re-applied if a bit was lost (a sandbox load restoring vars).
			const auto plan = plan::BorrowPlan(bits, a_mode);
			if (plan.empty()) {
				return "";
			}
			std::string results;
			for (const auto& stmt : plan) {
				const auto r = a_call(std::format("local ok, e = pcall(function() {} end) return tostring(ok) .. (e and (' ' .. tostring(e)) or '')", stmt));
				results += std::format("{}{} -> {}", results.empty() ? "" : "; ", stmt.substr(stmt.find('.') + 1), r);
			}
			++g_borrow.reapplied;
			Log::Info("quiet: re-borrowed after a load ({}): {}", plan::DescribeBits(bits), results);
			return std::format("quiet: re-borrowed: {}", results);
		}

		// The put-back: the bits through Lua's setters, the counters raw, then the raw diff against the snapshot. Lua thread.
		std::string PutBack(const LuaCall& a_call, const char* a_why)
		{
			std::scoped_lock lock(g_borrowLock);
			if (!g_borrow.active) {
				return "quiet: nothing borrowed";
			}
			const auto now = ReadBits(a_call);
			const auto plan = plan::RestorePlan(g_borrow.snapBits, now);
			std::string results;
			for (const auto& stmt : plan) {
				const auto r = a_call(std::format("local ok, e = pcall(function() {} end) return tostring(ok) .. (e and (' ' .. tostring(e)) or '')", stmt));
				results += std::format("{}{} -> {}", results.empty() ? "" : "; ", stmt.substr(stmt.find('.') + 1), r);
			}
			// Her counters (moved in memory by use: the bond, the call count, the timer) back to the snapshot's bytes.
			int        written = 0;
			const auto block = Get<std::uintptr_t>(addr::kVarsPtr);
			for (const auto& [off, len] : plan::CounterSpans()) {
				if (block && *block && Put(*block + off, g_borrow.snap.data() + (off - plan::kBlockFirst), len)) {
					++written;
				}
			}
			// Her bits raw by mask (the setters above put the live ones back; the mission-start copies only this way).
			for (const auto& [off, mask] : plan::MaskedSpans()) {
				const auto now = block && *block ? Get<std::uint8_t>(*block + off) : std::nullopt;
				const auto want = plan::MergeMasked(now.value_or(0), g_borrow.snap[off - plan::kBlockFirst], mask);
				if (now && *now != want && Put(*block + off, &want, 1)) {
					++written;
				}
			}
			std::array<std::uint8_t, plan::kBlockBytes> after{};
			std::uint8_t                                 st = 0;
			std::string                                  diffText = "the block couldn't be read";
			bool                                         clean = false;
			if (ReadBlock(after, st)) {
				const auto diff = plan::DiffBlock(g_borrow.snap.data(), after.data(), g_borrow.staticSnap, st);
				clean = plan::DiffClean(diff);
				diffText = plan::DescribeDiff(diff);
			}
			const auto bits = ReadBits(a_call);
			g_bits = bits;
			g_borrow.active = false;
			g_lastDiff = diffText;
			const auto line = std::format("quiet: put back ({}): {}; {} counter spans / masked bytes written; now {}; raw diff: {} -- {}", a_why, results.empty() ? "no bit to set" : results,
				written, plan::DescribeBits(bits), diffText, clean ? "CLEAN (no byte of Quiet's differs)" : "QUIET'S BYTES DIFFER");
			if (clean) {
				Log::Info("{}", line);
			} else {
				Log::Warn("{}", line);
			}
			return line;
		}

		// Lua: send her a SendCommand (her object id from the class, never a stale id).
		std::string SendCommandLua(const LuaCall& a_call, const std::string& a_fields)
		{
			return a_call(std::format(
				"local id = GameObject.GetGameObjectIdByIndex('TppBuddyQuiet2', 0) if type(id) ~= 'number' or id == GameObject.NULL_ID then return 'no Quiet' end "
				"local ok, r = pcall(GameObject.SendCommand, id, {{ {} }}) return tostring(ok) .. ' ' .. tostring(r)",
				a_fields));
		}
	}

	namespace
	{
		// The call menu's buddy entries ([[0x140BFF050()+0x98]+0xE8], FUN_14110f340 builds the menu from it: entry ids +0x78..+0x84, their
		// "live" bytes +0x170..+0x173; +0x70 the menu's target). Her standby leaves Fire / Take aim greyed even with a marked enemy (live
		// M1): only her sniping state (at one of MGSV's snipe points) makes them live. With her stand-in marked, the guest makes Fire (1/2)
		// and Take aim (3) live itself -- only when the entries read as Quiet's (+0x78 1 or 2, +0x7C 3).
		// MGSV's call menu offers Quiet's Fire live now (the buddy system's entry 0 is Fire (1/2) and its live byte +0x170 is set): only
		// her sniping state at one of MGSV's snipe points sets it, with her target acquired (live M1). nullopt = unreadable / not hers.
		std::optional<bool> FireLive()
		{
			if (!g_menuOk) {
				return std::nullopt;
			}
			const auto obj = reinterpret_cast<AccessorFn>(addr::kMessageBoxAccessor.va)();
			const auto p98 = obj ? Get<std::uintptr_t>(obj + 0x98) : std::nullopt;
			const auto menu = p98 && *p98 ? Get<std::uintptr_t>(*p98 + 0xE8) : std::nullopt;
			const auto e0 = menu && *menu ? Get<std::uint32_t>(*menu + 0x78) : std::nullopt;
			const auto live = menu && *menu ? Get<std::uint8_t>(*menu + 0x170) : std::nullopt;
			if (!e0 || !live || (*e0 != 1 && *e0 != 2)) {
				return std::nullopt;
			}
			return *live != 0;
		}

		bool ForceMenuFire()
		{
			if (!g_menuOk) {
				return false;
			}
			const auto obj = reinterpret_cast<AccessorFn>(addr::kMessageBoxAccessor.va)();
			const auto p98 = obj ? Get<std::uintptr_t>(obj + 0x98) : std::nullopt;
			const auto menu = p98 && *p98 ? Get<std::uintptr_t>(*p98 + 0xE8) : std::nullopt;
			if (!menu || !*menu) {
				return false;
			}
			const auto e0 = Get<std::uint32_t>(*menu + 0x78), e1 = Get<std::uint32_t>(*menu + 0x7C);
			if (!e0 || !e1 || (*e0 != 1 && *e0 != 2) || *e1 != 3) {
				return false;
			}
			const auto live = Get<std::array<std::uint8_t, 2>>(*menu + 0x170);
			if (live && (*live)[0] == 1 && (*live)[1] == 1) {
				return true;
			}
			const std::uint8_t on[2] = { 1, 1 };
			return Put(*menu + 0x170, on, 2);
		}
	}

	// ======================================================================================================================
	void Install()
	{
		const bool handler = Matches(addr::kQuietMessageHandler, "quiet message handler") && Matches(addr::kQuietHandlerSlot0, "quiet handler slot") &&
		                     Matches(addr::kQuietHandlerSlot1, "quiet handler slot (2)");
		g_recordOk = Matches(addr::kQuietRecordFire, "quiet order record (fire case)") && Matches(addr::kQuietRecordProcessor, "quiet order record (processor)") &&
		             Matches(addr::kQuietRecordHitSend, "quiet order record (hit sender)");
		g_hitOk = Matches(addr::kQuietHitSend, "quiet hit sender") && Matches(addr::kQuietHitSoldierOnly, "quiet hit: soldiers only") &&
		          Matches(addr::kQuietHitMessage, "quiet hit message") && Matches(addr::kQuietShotCall, "quiet shot's call of the hit sender") &&
		          Matches(addr::kQuietShotTarget, "quiet shot's target");
		g_cantOk = Matches(addr::kQuietNoTarget, "quiet can't");
		g_commandsOk = Matches(addr::kBuddyCommandQuietFire, "buddy command: quiet fire") && Matches(addr::kBuddyCommandQuietAim, "buddy command: quiet aim") &&
		               Matches(addr::kBuddyCommandQuietCover, "buddy command: quiet cover") && Matches(addr::kBuddyCommandQuietCease, "buddy command: quiet cease") &&
		               Matches(addr::kBuddyCommandQuietShootThis, "buddy command: quiet shoot this") && Matches(addr::kBuddyCommandQuietTalk, "buddy command: quiet talk");
		g_varsOk = Matches(addr::kVarBuddyQuietEquipType);
		g_staticOk = Matches(addr::kSetDisableBuddyStatic, "SetDisableBuddyType's static byte");
		g_menuOk = Matches(addr::kMessageBoxAccessor, "message box accessor (the call menu's entries)");
		const bool orderSites = Matches(addr::kQuietFireTarget, "quiet fire target") && Matches(addr::kQuietOrderDelay, "quiet order delay") &&
		                        Matches(addr::kQuietOrderProcessor, "quiet order processor") && Matches(addr::kQuietOrderPickTarget, "quiet order pick target");
		g_sites = handler && g_recordOk && g_hitOk && g_commandsOk && orderSites && g_varsOk ? 1 : 0;
		if (!g_sites) {
			Log::Warn("quiet: MGSV's Quiet code differs from what this build was written for -- the Quiet layer stays off ([Quiet] ignored)");
			return;
		}
		const bool h0 = Hooks::Create("quiet message handler", reinterpret_cast<void*>(addr::kQuietMessageHandler.va), &HandlerDetour, &g_handler);
		const bool h1 = Hooks::Create("quiet hit sender", reinterpret_cast<void*>(addr::kQuietHitSend.va), &HitDetour, &g_hit);
		const bool h2 = g_cantOk && Hooks::Create("quiet can't", reinterpret_cast<void*>(addr::kQuietNoTarget.va), &CantDetour, &g_cant);
		if (!h0 || !h1) {
			g_sites = 0;
			Log::Warn("quiet: a detour was refused (handler {}, hit {}) -- the Quiet layer stays off", h0, h1);
			return;
		}
		const auto& q = Cfg().quiet;
		Log::Info("quiet: sites ok (static byte {}, can't hook {}); [Quiet] Enabled {} Borrow {} BorrowListInIdroid {} Mirror {} Treadmill {} Orders {} MarkProxy {} "
		          "KeepMarks {} FireOnlyOnOrders {} Cover {} TakeOverTalk {} RestAtGrace {} SitPose {} SitPlacement {}",
			g_staticOk, h2, q.enabled, static_cast<int>(q.borrow), q.borrowListInIdroid, q.mirror, q.treadmill, q.orders, q.markProxy, q.keepMarks,
			q.fireOnlyOnOrders, q.cover, q.takeOverTalk, q.restAtGrace, static_cast<int>(q.sitPose), static_cast<int>(q.sitPlacement));
	}

	// ---- game thread ---------------------------------------------------------------------------------------------------------
	void Update(const state::LuaView& a_lua, const FrameIn& a_in)
	{
		const auto& q = Cfg().quiet;
		const bool  out = g_sites == 1 && QuietOut(a_lua);
		g_feet.reset();
		g_snakeX.store(a_in.root.x, std::memory_order_relaxed);
		g_snakeZ.store(a_in.root.z, std::memory_order_relaxed);
		// [Quiet] MirrorRegion: the game thread's own copy (`!quiet set region` hands a new one over under the lock).
		if (!g_regionInit) {
			g_regionInit = true;
			g_region = q.mirrorRegion;
		}
		{
			std::scoped_lock lock(g_holdLock);
			if (g_regionNew) {
				g_region = std::move(*g_regionNew);
				g_regionNew.reset();
			}
		}
		g_atPost.store(false, std::memory_order_relaxed);
		// The treadmill offset k = root - odometer (odometer space stays put through the treadmill's jumps).
		g_kNow = proxy::plan::TreadmillOffset(a_in.root, a_in.odo);
		g_k.Record(a_in.nowMs, g_kNow);
		if ((g_kNow - g_kLast).Length() > 1.0f) {
			g_kMovedMs = a_in.nowMs;
		}
		g_kLast = g_kNow;
		if (out && a_lua.buddyId != g_quietId) {
			g_finder.Forget("another Quiet");
			g_quietId = a_lua.buddyId;
			g_pendingShift.reset();
		}
		if (!out) {
			g_quietId = 0xFFFF;
			g_lastSeen.reset();
			g_rootX.store(NAN);
		}

		// A new host session (Elden Ring restarted, or another host stand-in) starts its grace and post sequences over: what was
		// taken from the last one is forgotten (live M3: a recorder's rest #1 was ignored after fake_er's rest #1).
		{
			static std::uint32_t generation = 0;
			const auto           now = link::AttachGeneration();
			if (now != generation) {
				generation = now;
				g_sit.Reset();
				g_hold = {};
			}
		}
		// The host's view: its layer, post and grace (MGSV world as of the GuestState it consumed: into odometer space with k then).
		{
			proto::HostState hs{};
			if (link::HostAlive() && link::ReadHostState(hs)) {
				const Vec3 kThen = g_k.At(a_in.nowMs > 33 ? a_in.nowMs - 33 : 0);
				HostView   h;
				h.valid = true;
				h.layer = (hs.flags & proto::kHostQuietLayer) != 0;
				h.atGrace = (hs.flags & proto::kHostAtGrace) != 0;
				h.postSeq = hs.buddyPostSeq;
				h.graceSeq = hs.graceSeq;
				const Vec3 post{ hs.buddyPost[0], hs.buddyPost[1], hs.buddyPost[2] };
				h.postValid = h.layer && hs.buddyPostSeq != 0 && post.Finite() && std::isfinite(hs.buddyPostYaw);
				h.postOdo = post - kThen;
				h.postYaw = hs.buddyPostYaw;
				const Vec3 seat{ hs.graceSeat[0], hs.graceSeat[1], hs.graceSeat[2] };
				const Vec3 centre{ hs.graceCentre[0], hs.graceCentre[1], hs.graceCentre[2] };
				h.seatValid = h.atGrace && hs.graceSeq != 0 && seat.Finite() && std::isfinite(hs.graceSeatYaw);
				h.centreValid = h.atGrace && centre.Finite();
				h.seatOdo = seat - kThen;
				h.centreOdo = centre - kThen;
				h.seatYaw = hs.graceSeatYaw;
				g_host = h;
				const int layer = h.layer ? 1 : 0;
				if (g_hostLayer.exchange(layer) != layer) {
					Log::Info("quiet: the host's Quiet layer is {}", layer ? "on (kHostQuietLayer): it picks her posts and targets" : "off: Quiet is MGSV's own (0.8.0)");
				}
				g_atGrace.store(h.atGrace);
			} else if (!link::HostAlive()) {
				g_host = {};
				g_hostLayer.store(0);
				g_atGrace.store(false);
			}
		}

		// Her place: her skin's root when fresh, else GetPosition. Heading from the skin.
		Vec3       skinAt;
		float      skinYaw = 0.0f;
		const bool skin = out && skeleton::BuddyPose(skinAt, skinYaw, 300);
		g_skinYaw.store(skin ? skinYaw : NAN);
		const std::optional<Vec3> ref = skin ? std::optional<Vec3>(skinAt) : out && a_lua.buddyPosValid ? std::optional<Vec3>(a_lua.buddyPos) : std::nullopt;
		static bool holdTried = false;
		if (out && !hold::Available() && !holdTried) {
			holdTried = true;
			hold::Init();
		}
		const bool layer = LayerOn() && HostLayer();
		Holder     want;
		{
			std::scoped_lock lock(g_holdLock);
			if (g_consoleHoldOff) {
				g_consoleHoldOff = false;
				g_consoleHold.reset();
				if (g_hold.why == HoldWhy::kConsole) {
					g_hold = {};
				}
			}
			if (g_consoleHold) {
				want = *g_consoleHold;
			}
		}
		// The grace seat first, then the host's post (only with its layer and [Quiet] Mirror), else the console's point.
		plan::SitInput sit;
		sit.out = out && layer;
		sit.canRest = q.restAtGrace;
		sit.atGrace = g_host.atGrace;
		sit.graceSeq = g_host.graceSeq;
		sit.seat = g_host.seatValid ? g_host.seatOdo + g_kNow : Vec3{ NAN, NAN, NAN };
		sit.centre = g_host.centreValid ? g_host.centreOdo + g_kNow : Vec3{ NAN, NAN, NAN };
		sit.seatYaw = g_host.seatYaw;
		const bool wantsPlace = out && (layer || want.why == HoldWhy::kConsole || g_scanWanted);
		const bool placeable = wantsPlace && buddyloc::SitesOk("quiet") && hold::Available();
		g_finder.Update(ref, a_in.nowMs, placeable);
		const auto loc = placeable ? g_finder.Confirmed() : 0;
		auto       feet = loc ? buddyloc::Finder::Feet(loc) : std::nullopt;
		sit.her = feet ? feet : ref;
		// Placed only while Snake is ready on foot (not climbing at MGSV's staging site, in Tarnished mode, in a vehicle or on the Fulton
		// anchor: review -- FrameIn.ready was computed and never read); the seat waits meanwhile (the host's deadline drops a late one).
		sit.placeable = loc != 0 && a_in.ready;
		const auto pose = q.sitPose == QuietSitPose::kKneel ? plan::SitPose::kKneel : q.sitPose == QuietSitPose::kSeat ? plan::SitPose::kSeat : plan::SitPose::kWatch;
		sit.moveSent = g_sitMoveSent.load();
		sit.nowMs = a_in.nowMs;
		const auto wasSeated = g_sit.Seated();
		const auto sitStep = g_sit.Update(sit, pose);
		if (sitStep.moveTo) {
			g_sitOrderMs = a_in.nowMs;
			std::scoped_lock lock(g_sitLock);
			g_sitMove = SitMove{ sitStep.moveId, *sitStep.moveTo, sitStep.moveAim, false };
		}
		if (g_sit.Seated() && g_sit.Seated() != wasSeated) {
			g_sitPlaced.store(a_in.nowMs);
			Log::Info("quiet: seated at the grace seat (rest {}, settled {} ms after her order): her rifle {}", g_sit.Seated(), a_in.nowMs - g_sitOrderMs,
				pose == plan::SitPose::kWatch ? "kept up (on watch)" : "upright, not aiming");
		}
		if (sitStep.getUp) {
			g_upPending = true;
			g_upSince = a_in.nowMs;
			Log::Info("quiet: up from the grace seat ({}) -- she keeps her rest at her posts until she is unseen (on the move): her order then", sitStep.why);
		}
		if (g_upPending && (!out || !layer)) {
			g_upPending = false;
		}
		if (g_upPending && !g_sit.Busy()) {
			// Up again (her aim on) only while she has no post (unseen); placed at her posts meanwhile in her rest.
			if (!g_host.postValid && a_in.nowMs - g_upSince > 1500) {
				const Vec3 at = feet ? *feet : ref.value_or(a_in.root);
				Vec3       dir = at - a_in.root;  // 2.5 m on, away from Snake
				dir.y = 0.0f;
				if (dir.LengthXZ() < 0.1f) {
					dir = { 0.0f, 0.0f, 1.0f };
				}
				const Vec3 to = at + dir * (2.5f / dir.LengthXZ());
				g_upPending = false;
				g_upWait = ++g_upIds | 0x80000000u;
				g_upWaitSince = a_in.nowMs;
				{
					std::scoped_lock lock(g_sitLock);
					g_sitMove = SitMove{ g_upWait, to, true, true };
				}
				Log::Info("quiet: getting up while unseen (no post) -- her order to {}", V3(to));
			}
		}
		if (g_upWait && (g_sitMoveSent.load() == g_upWait || a_in.nowMs - g_upWaitSince > 2000 || !out)) {
			g_upWait = 0;
		}
		g_graceEcho.store(g_sit.Seated());
		if (g_sit.Busy() || g_upWait) {
			// The seat holds her (tolerance 0.3 m: SitHold places her itself); or her get-up order hasn't gone out yet (placed after it).
			want = {};
		} else if (layer && g_standIn.load()) {
			// A stand-in is out: she takes her shot from MGSV's own snipe point (live M1); the holder lets her go and puts her back after.
			want = {};
		} else if (layer && q.mirror && g_host.postValid) {
			const auto  centre = state::Centre();
			const Vec3  postNow = g_host.postOdo + g_kNow;
			const Vec3  to = plan::MirrorPoint(postNow, a_in.root, a_in.camPos, centre.value_or(a_in.root), g_region, a_in.root.y);
			want.why = HoldWhy::kPost;
			want.odo = to - g_kNow;
			want.yaw = g_host.postYaw;
			want.tolerance = std::max(0.5f, q.postTolerance);
			want.seq = g_host.postSeq;
		} else if (want.why != HoldWhy::kConsole) {
			want = {};
		}

		// Last frame's treadmill jump, now that Snake's root has landed: her by the same shift (also while not ready: it keeps her where
		// she was in odometer space).
		bool shifted = false, placed = false;  // this frame moved her on purpose: no "dash" for the cloak test below
		if (const auto shift = std::exchange(g_pendingShift, std::nullopt)) {
			if (feet && g_finder.Place(*feet, *feet + *shift, std::nullopt)) {
				++g_shifts;
				feet = *feet + *shift;
				shifted = true;
			} else if (++g_shiftFails <= 20) {
				Log::Warn("quiet: {}: her location object couldn't be moved", g_pendingWhy);
			}
		}
		std::optional<Vec3> test;
		{
			std::scoped_lock lock(g_holdLock);
			test = std::exchange(g_testShift, std::nullopt);
		}
		if (test) {
			if (!feet) {
				g_lastPlace = "console: not moved (no confirmed object)";
			} else {
				const bool ok = g_finder.Place(*feet, *feet + *test, std::nullopt);
				g_lastPlace = std::format("console: {} by {:.2f},{:.2f} from {}", ok ? "moved" : "NOT moved", test->x, test->z, V3(feet));
				if (ok) {
					feet = *feet + *test;
					placed = true;
				}
			}
			Log::Info("quiet: {}", g_lastPlace);
		}
		// The seat's placement (one frame, by her location object).
		if (sitStep.place && feet) {
			const bool ok = g_finder.Place(*feet, *sitStep.place, logic::YawQuat(sitStep.placeYaw));
			g_lastPlace = std::format("seat (rest {}): {} {} -> {} facing {:.0f} deg ({})", g_sit.Busy(), ok ? "placed" : "NOT placed", V3(feet), V3(sitStep.place),
				sitStep.placeYaw * 180.0f / std::numbers::pi_v<float>, sitStep.why);
			Log::Info("quiet: {}", g_lastPlace);
			ok ? ++g_places : ++g_placeFails;
			if (ok) {
				feet = *sitStep.place;
				placed = true;
			}
		}
		// The holder (post / console): re-placed past its tolerance; a new post or point placed at once.
		if (want.why != g_hold.why || want.seq != g_hold.seq || (want.odo - g_hold.odo).Length() > 0.05f) {
			const bool newPost = want.why != g_hold.why || want.seq != g_hold.seq;
			g_hold.why = want.why;
			g_hold.odo = want.odo;
			g_hold.yaw = want.yaw;
			g_hold.tolerance = want.tolerance;
			g_hold.seq = want.seq;
			g_hold.at = false;
			if (newPost) {
				g_hold.tries = 0;
			}
		}
		if (g_hold.why != HoldWhy::kNone && feet && a_in.ready) {  // (not while climbing, parked, in a vehicle or anchored: review)
			const Vec3  to{ g_hold.odo.x + g_kNow.x, a_in.root.y, g_hold.odo.z + g_kNow.z };  // on MGSV's ground (MirrorHeight ground, Q8)
			const float off = (*feet - to).LengthXZ();
			// Not while she drops in (MGSV's HALO deploy: 2 km up, live M3) or dashes cloaked: a placement doesn't hold then. A burst of
			// at most 40 tries, again 3 s after the last one (her AI going back to a snipe point of its own wins a burst: live M1).
			static std::uint64_t lastTry = 0;
			const bool           settled = std::fabs(feet->y - a_in.root.y) < 15.0f && a_in.nowMs >= g_cloakUntil.load() && a_in.nowMs - g_kMovedMs > kKSettleMs;
			if (g_hold.tries >= 40 && a_in.nowMs - lastTry > 3000) {
				g_hold.tries = 0;
			}
			g_hold.at = off <= g_hold.tolerance;  // measured, never assumed from a placement
			if (!g_hold.at && settled && g_hold.tries < 40) {
				++g_hold.tries;
				lastTry = a_in.nowMs;
				const Vec3 dst{ to.x, to.y, to.z };
				const bool ok = g_finder.Place(*feet, dst, logic::YawQuat(g_hold.yaw));
				ok ? ++g_places : ++g_placeFails;
				if (g_places + g_placeFails <= 200 || !ok) {
					Log::Info("quiet: held at the {} ({:.1f} m off): {} {} -> {}", g_hold.why == HoldWhy::kPost ? std::format("host's post {}", g_hold.seq) : std::string("console's point"),
						off, ok ? "placed" : "NOT placed", V3(feet), V3(dst));
				}
				if (ok) {
					feet = dst;
					placed = true;
				}
			}
		}
		g_atPost.store(g_hold.why == HoldWhy::kPost && g_hold.at && out);
		// Away for an order (she snipes from MGSV's snipe point): Elden Ring hides her as cloaked (MGSV's Quiet is unseen there anyway).
		g_away.store(out && layer && g_standIn.load() && !g_sit.Busy());
		g_postEcho.store(g_hold.why == HoldWhy::kPost && g_hold.at && out ? g_hold.seq : 0u);

		// The cloak: MGSV draws her not at all while she dashes (live: 20-33 m/s); her speed tells it.
		const std::optional<Vec3> seen = feet ? feet : ref;
		if (seen && g_lastSeen && a_in.dt > 0.0f) {
			const float speed = (*seen - *g_lastSeen).Length() / a_in.dt;
			if (speed > 12.0f && speed < 400.0f && !shifted && !placed) {  // (review: her own treadmill shift read as a dash: 300 ms hidden)
				g_cloakUntil.store(a_in.nowMs + 300);
			}
		}
		g_lastSeen = seen;
		// The call menu's Fire / Take aim live while her stand-in is marked (`!quiet menufire`, or [Quiet] Orders + MarkProxy with the layer).
		{
			const int  mf = g_menuFire.load();
			// (live M1: forcing the entry makes MGSV offer Fire, but she can't fire from her standby -- a test switch only)
			const bool want = out && g_standIn.load() && mf == 1;
			if (want && ForceMenuFire()) {
				g_menuForced.fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (seen) {
			g_rootX.store(seen->x);
			g_rootY.store(seen->y);
			g_rootZ.store(seen->z);
		}
		if (feet) {
			g_feet = feet;
		}
		{
			Snapshot snap;
			snap.feet = g_feet;
			snap.holdWhy = g_hold.why;
			snap.holdAt = g_hold.odo + g_kNow;
			snap.holdThere = g_hold.at;
			snap.lastPlace = g_lastPlace;
			snap.candidate = g_finder.Candidate();
			snap.confirmed = g_finder.Confirmed() != 0;
			snap.scans = g_finder.Scans();
			std::scoped_lock lock(g_holdLock);
			g_snap = std::move(snap);
		}
	}

	bool Shift(const Vec3& a_shift, const char* a_why)
	{
		const auto& q = Cfg().quiet;
		const bool  probe = g_hold.why == HoldWhy::kConsole;
		if (g_sites != 1 || !((q.treadmill && LayerOn() && HostLayer()) || probe) || !g_finder.Confirmed() || !a_shift.Finite()) {
			return false;
		}
		g_pendingShift = g_pendingShift ? *g_pendingShift + a_shift : a_shift;
		g_pendingWhy = a_why;
		return true;
	}

	std::optional<Vec3> Position() { return g_feet; }

	std::optional<float> Yaw()
	{
		const float y = g_skinYaw.load();
		return std::isfinite(y) ? std::optional<float>(y) : std::nullopt;
	}

	// ---- the Lua thread ------------------------------------------------------------------------------------------------------
	std::string BeforeDeploy(bool a_guarded, const LuaCall& a_call)
	{
		// Called every boot tick while her deployment runs: once every 2 s is enough, and a line only when it changes.
		static std::uint64_t at = 0;
		static std::string   last;
		const auto           now = ::GetTickCount64();
		if (now - at < 2000) {
			return "";
		}
		at = now;
		if (g_sites != 1 || Mode() == plan::BorrowMode::kOff) {
			if (g_sites == 1 && !g_bootLogged) {
				g_bootLogged = true;
				const auto bits = ReadBits(a_call);
				g_bits = bits;
				Log::Info("quiet: save state {} ({}); [Quiet] Borrow = off", plan::StateName(plan::Classify(bits)), plan::DescribeBits(bits));
			}
			return "";
		}
		g_bootLogged = true;
		if (!HostLayer()) {
			// Not without the host's Quiet layer (review): she would be 0.8.0's Quiet. Borrowed once the layer is on (LuaStep).
			g_borrowPending = !g_borrowByHand;
			const std::string r = "quiet: her borrow waits for Elden Ring's Quiet layer ([Quiet] enabled and posts in PhantomRingHost.ini) -- without it "
			                      "she would be 0.8.0's Quiet";
			if (r != last) {
				Log::Info("{}", r);
			}
			last = r;
			return r;
		}
		const auto r = DoBorrow(a_guarded, Mode(), a_call, "before her deployment");
		if (!r.empty() && r != last) {
			Log::Info("{}", r);
		}
		last = r;
		return r;
	}

	void AtReady(bool a_guarded, const LuaCall& a_call)
	{
		if (g_sites != 1) {
			return;
		}
		// After a sandbox load MGSV may have re-created her with its own EnableFire / EnableDying defaults (same object id 0x2400, so
		// LuaStep's id check can't tell): send them again (review: NoDying stayed at MGSV's default for the session).
		g_enableFor = 0xFFFF;
		g_fireSent = g_aimSent = g_dyingSent = -1;
		g_borrowByHand = false;  // a new load: the ini's borrow applies again
		g_strikeAgain.store(true);
		bool active = false;
		{
			std::scoped_lock bl(g_borrowLock);
			active = g_borrow.active;
		}
		if (active) {
			// Borrowed earlier this session (the ini, the console, the iDroid listing): re-applied if this load dropped a bit.
			const auto r = DoBorrow(a_guarded, Mode() == plan::BorrowMode::kOff ? plan::BorrowMode::kAlways : Mode(), a_call, "after a load");
			if (!r.empty()) {
				Log::Info("{}", r);
			}
			return;
		}
		if (!Cfg().quiet.borrowListInIdroid || Mode() == plan::BorrowMode::kOff) {
			if (!g_bootLogged) {
				g_bootLogged = true;
				const auto bits = ReadBits(a_call);
				g_bits = bits;
				Log::Info("quiet: save state {} ({}); her borrow {}", plan::StateName(plan::Classify(bits)), plan::DescribeBits(bits),
					Mode() == plan::BorrowMode::kOff ? "off ([Quiet] Borrow = off)" : "only when she is asked for ([Quiet] BorrowListInIdroid = false)");
			}
			return;
		}
		g_bootLogged = true;
		if (!HostLayer()) {
			// The borrow waits for the host's Quiet layer (review: with it off she would be 0.8.0's unguarded Quiet on every save).
			g_borrowPending = true;
			if (!g_borrowPendingLogged) {
				g_borrowPendingLogged = true;
				const auto bits = ReadBits(a_call);
				g_bits = bits;
				Log::Info("quiet: save state {} ({}); her borrow waits for Elden Ring's Quiet layer ([Quiet] enabled and posts) -- then MGSV's BUDDY "
				          "SUPPORT lists her",
					plan::StateName(plan::Classify(bits)), plan::DescribeBits(bits));
			}
			return;
		}
		g_borrowPending = false;
		const auto r = DoBorrow(a_guarded, Mode(), a_call, "at the sandbox's start, so MGSV's BUDDY SUPPORT lists her");
		if (!r.empty()) {
			Log::Info("{}", r);
		}
	}

	void LuaStep(std::uint64_t a_nowMs, bool a_ready, bool a_guarded, const LuaCall& a_call)
	{
		if (g_sites != 1) {
			return;
		}
		const auto& q = Cfg().quiet;
		// Console Lua work first.
		{
			std::vector<std::string> work;
			{
				std::scoped_lock lock(g_luaLock);
				work.swap(g_luaQueue);
			}
			for (const auto& w : work) {
				const auto r = a_call(w);
				Log::Info("quiet: console Lua: {} -> {}", w.substr(0, 160), r);
				std::scoped_lock lock(g_luaLock);
				g_luaLast = r;
			}
		}
		switch (g_borrowAsk.exchange(0)) {
		case 1: {
			g_borrowByHand = false;
			const auto r = DoBorrow(a_guarded, Mode() == plan::BorrowMode::kOff ? plan::BorrowMode::kAlways : Mode(), a_call, "the console");
			Log::Info("{}", r.empty() ? std::string("quiet: borrow: nothing to re-apply") : r);
			std::scoped_lock lock(g_luaLock);
			g_luaLast = r;
			break;
		}
		case 2: {
			g_borrowByHand = true;  // not borrowed again by itself (the ini's pending borrow) until `!quiet borrow` or the next load
			g_borrowPending = false;
			const auto r = PutBack(a_call, "the console");
			std::scoped_lock lock(g_luaLock);
			g_luaLast = r;
			break;
		}
		case 3: {
			const auto bits = ReadBits(a_call);
			g_bits = bits;
			const auto r = std::format("quiet: save state {} ({})", plan::StateName(plan::Classify(bits)), plan::DescribeBits(bits));
			Log::Info("{}", r);
			std::scoped_lock lock(g_luaLock);
			g_luaLast = r;
			break;
		}
		case 4: {
			std::array<std::uint8_t, plan::kBlockBytes> now{};
			std::uint8_t                                 st = 0;
			std::string                                  r = "quiet: diff: nothing borrowed";
			{
				std::scoped_lock bl(g_borrowLock);
				if (g_borrow.active && ReadBlock(now, st)) {
					const auto diff = plan::DiffBlock(g_borrow.snap.data(), now.data(), g_borrow.staticSnap, st);
					r = std::format("quiet: diff against the snapshot (borrowed): {}", plan::DescribeDiff(diff));
				}
			}
			Log::Info("{}", r);
			std::scoped_lock lock(g_luaLock);
			g_luaLast = r;
			break;
		}
		default:
			break;
		}
		// A guard lost, or Elden Ring gone for 3 s, with her borrowed: put back now (memory only; nothing may reach the save).
		{
			static std::uint64_t hostGoneSince = 0;
			hostGoneSince = link::HostAlive() ? 0 : hostGoneSince ? hostGoneSince : a_nowMs;
			bool active = false;
			{
				std::scoped_lock bl(g_borrowLock);
				active = g_borrow.active;
			}
			if (active && !a_guarded) {
				Log::Warn("quiet: a save guard was lost with her borrowed -- putting her bits back now");
				PutBack(a_call, "a save guard was lost");
			} else if (active && hostGoneSince && a_nowMs - hostGoneSince > 3000) {
				PutBack(a_call, "Elden Ring is gone");
				g_borrowPending = !g_borrowByHand;  // borrowed again once Elden Ring (and its Quiet layer) is back (review)
			}
			// The ini's borrow that waited for the host's Quiet layer (or for Elden Ring to come back): now, while guarded, at most every
			// 5 s; a state with nothing to borrow (present, or not wanted under Borrow) ends the wait.
			if (!active && g_borrowPending && !g_borrowByHand && a_guarded && HostLayer() && Mode() != plan::BorrowMode::kOff &&
				a_nowMs - g_borrowTriedMs >= 5000) {
				g_borrowTriedMs = a_nowMs;
				const auto         r = DoBorrow(a_guarded, Mode(), a_call, "Elden Ring's Quiet layer is on");
				static std::string lastR;
				std::scoped_lock   bl(g_borrowLock);
				if (g_borrow.active || r.find("nothing to borrow") != std::string::npos || r.find("off:") != std::string::npos) {
					g_borrowPending = false;
				}
				if (!r.empty() && r != lastR) {
					Log::Info("{}", r);
				}
				lastR = r;
			}
		}
		if (!a_ready) {
			return;
		}
		const auto lua = state::Lua();
		const bool out = QuietOut(lua);
		// QUIET SCOUT / ATTACK greyed out while she is out with the host's layer on (her place is Elden Ring's post), active otherwise:
		// they are how the player deploys her from the iDroid (live 0.9.0 integration: greyed from the boot on, a greyed entry can't be
		// chosen, so no player could deploy her -- the borrow listed her for nothing).
		{
			static int strikeSent = -1;
			const int  want = LayerOn() && HostLayer() && Cfg().quiet.strikeMgsvPosts && out ? 1 : 0;
			if (want != strikeSent || g_strikeAgain.exchange(false)) {
				strikeSent = want;
				const auto r = a_call(std::format(
					"local m = type(TppTerminal) == 'table' and TppTerminal.MBDVCMENU or nil if type(m) ~= 'table' then return 'no TppTerminal.MBDVCMENU' end "
					"local out = '' for _, n in ipairs({{ 'MSN_BUDDY_QUIET_SCOUT', 'MSN_BUDDY_QUIET_ATTACK' }}) do local item = m[n] "
					"if item ~= nil then local ok = pcall(TppUiCommand.SetMbTopMenuItemActive, item, {}) out = out .. n .. '=' .. tostring(ok) .. ' ' "
					"else out = out .. n .. '=missing ' end end return out",
					want ? "false" : "true"));
				Log::Info("quiet: the iDroid's QUIET SCOUT / ATTACK {}: {}", want ? "greyed out (she is out with the host's layer on)" : "active (they deploy her)", r);
			}
		}
		// Her weapon: vars.buddyQuietEquipType, and her live weapon [[sys+0x70]+0x30 + index*2]; a hit's attack id overrides.
		if (static std::uint64_t at = 0; a_nowMs - at >= 1000) {
			at = a_nowMs;
			const int equip = VarU16(addr::kVarBuddyQuietEquipType.offset).value_or(0xFFFF);
			if (g_equip.exchange(equip) != equip) {
				Log::Info("quiet: her equipment var buddyQuietEquipType = {} ({})", equip, plan::WeaponName(plan::WeaponOfEquip(equip)));
				g_weapon.store(plan::WeaponOfEquip(equip));  // a hit's attack id corrects it
			}
			if (const auto sys = out ? g_sys.load() : 0) {
				const auto comp = Get<std::uintptr_t>(sys + 0x70);
				const auto live = comp && *comp ? Get<std::uint16_t>(*comp + 0x30 + std::uintptr_t(g_index.load()) * 2) : std::nullopt;
				const int  lw = live ? *live : -1;
				if (g_liveWeapon.exchange(lw) != lw) {
					Log::Info("quiet: her live weapon (her weapon component +0x30) = {}", lw);
				}
				if (const auto rec = Record()) {
					if (const auto r0 = Get<std::uint64_t>(rec)) {
						const bool cover = (*r0 & 1) != 0;
						if (g_cover.exchange(cover) != cover) {
							Log::Info("quiet: MGSV's Cover me is {}", cover ? "ON (her order record's bit 0)" : "off");
						}
					}
				}
			}
		}
		if (!out) {
			g_enableFor = 0xFFFF;
			g_fireSent = g_aimSent = g_dyingSent = -1;
			{
				// A get-up (or a sit) queued for a Quiet who is no longer out is never delivered to a later one (review).
				std::scoped_lock lock(g_sitLock);
				g_sitMove.reset();
			}
			if (g_job.Busy()) {
				const auto step = g_job.Abort("Quiet is not out");
				if (step.event) {
					SendShot(*step.event, g_job.Handle(), 0, step.event == proto::kQuietLost ? static_cast<std::uint32_t>(step.lost) : 0u, step.why);
				}
			}
			if (g_standIn.exchange(false) && proxy::DedicatedBuddy() == state::kBuddyTypeQuiet) {
				proxy::Release("Quiet is not out");
			}
			g_jobBusy.store(false);
			g_engaged.store(false);
			capture::hud::SetHideQuietMarkers(false);
			return;
		}
		if (lua.buddyId != g_enableFor) {
			g_enableFor = lua.buddyId;
			g_fireSent = g_aimSent = g_dyingSent = -1;  // a new Quiet: her Enable* states are MGSV's defaults again
		}
		const bool layer = LayerOn() && HostLayer();

		// ---- the sit's stand-down / get-up (§3.9) ----
		std::optional<SitMove> mv;
		{
			std::scoped_lock lock(g_sitLock);
			mv = std::exchange(g_sitMove, std::nullopt);
		}
		if (mv) {
			// Her own order (MoveToPosition: aim off = she rests with the rifle upright on arrival; quiet.md §2.10 Q2), announcements off
			// first (no "Quiet: On the move" in the HUD layer at the grace); the game thread places her at the point right after.
			const auto r0 = SendCommandLua(a_call, mv->getUp ? "id = 'EnableAnnounceLog', enable = true" : "id = 'EnableAnnounceLog', enable = false");
			const auto r1 = mv->getUp ? SendCommandLua(a_call, "id = 'EnableAim', enable = true") : std::string("-");
			const auto r2 = SendCommandLua(a_call, std::format("id = 'MoveToPosition', position = Vector3({:.3f}, {:.3f}, {:.3f}), rotationY = 0, disableAim = {}, index = 0",
				mv->to.x, mv->to.y, mv->to.z, mv->aim ? "false" : "true"));
			g_aimSent = mv->aim ? 1 : 0;
			if (mv->id) {
				g_sitMoveSent.store(mv->id);
			}
			Log::Info("quiet: {} her order to {} (aim {}): announce -> {}, aim -> {}, MoveToPosition -> {}", mv->getUp ? "getting up:" : "the sit:", V3(mv->to),
				mv->aim ? "on" : "off", r0, r1, r2);
		}
		// ---- FireOnlyOnOrders / NoDying ----
		if (layer) {
			// Her fire on only while a stand-in of hers is out (the order window: MGSV's Fire needs it live before the player can click),
			// off otherwise: nothing of MGSV's (the figure at the terminal, Q17) is ever her target on her own.
			// A send counts only when MGSV took it ("true ..."): else -1 stays and the next step tries again (review). Both are sent again
			// every kEnableEveryMs (idempotent), quietly, so a Quiet MGSV re-created behind our back gets them too.
			const int  fire = q.fireOnlyOnOrders ? (g_standIn.load() || g_job.Busy() ? 1 : 0) : -1;
			const bool again = a_nowMs - g_enableAt >= kEnableEveryMs;
			if (again) {
				g_enableAt = a_nowMs;
			}
			if (fire >= 0 && (fire != g_fireSent || again) && !g_sit.Busy()) {
				const auto r = SendCommandLua(a_call, fire ? "id = 'EnableFire', enable = true" : "id = 'EnableFire', enable = false");
				const bool ok = r.starts_with("true");
				if (fire != g_fireSent || !ok) {
					Log::Info("quiet: EnableFire {} ({}) -> {}{}", fire ? "true" : "false", fire ? "an order" : "FireOnlyOnOrders", r, ok ? "" : " -- tried again");
				}
				g_fireSent = ok ? fire : -1;
			}
			if (q.noDying && (g_dyingSent != 0 || again)) {
				const auto r = SendCommandLua(a_call, "id = 'EnableDying', enable = false");
				const bool ok = r.starts_with("true");
				if (g_dyingSent != 0 || !ok) {
					Log::Info("quiet: EnableDying false (NoDying) -> {}{}", r, ok ? "" : " -- tried again");
				}
				g_dyingSent = ok ? 0 : -1;
			}
		}

		// ---- the order job (§3.5) ----
		static std::vector<proto::ActorRecord> recs(proto::kMaxActors), fresh(proto::kMaxActors);
		static std::uint32_t                   n = 0;
		static std::uint64_t                   tableMs = 0;
		const bool                             ordersOn = layer && q.orders && !g_ordersOff.load();
		if (lua.flags & state::kLBinocle) {
			g_lastBinocMs = a_nowMs;
		}
		const int cmd = g_playerOrder.exchange(-1);
		if (ordersOn && (cmd > 0 || g_job.Busy() || a_nowMs - tableMs >= 100)) {
			std::uint32_t fn = 0, seq = 0;
			if (link::ReadActors(fresh.data(), proto::kMaxActors, fn, seq)) {
				recs.swap(fresh);
				n = fn;
				tableMs = a_nowMs;
			} else if (!link::HostAlive() || a_nowMs - tableMs > 2000) {
				n = 0;
			}
		}
		const auto findRecord = [&](std::uint64_t a_h) -> const proto::ActorRecord* {
			for (std::uint32_t i = 0; i < n; ++i) {
				if (recs[i].handle == a_h) {
					return &recs[i];
				}
			}
			return nullptr;
		};
		std::uint64_t pick = 0;
		if (ordersOn) {
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto& r = recs[i];
				if ((r.flags & proto::kActorQuietTarget) && (r.flags & proto::kActorHostile) && !(r.flags & (proto::kActorDead | proto::kActorBoss)) && r.handle) {
					pick = r.handle;
					break;
				}
			}
			if (pick) {
				g_pickSeen = pick;
				g_pickSeenMs = a_nowMs;
			}
		}
		const auto cancel = [&]() -> const char* {
			if (!layer) {
				return "the host's layer is off";
			}
			if (g_atGrace.load()) {
				return "Snake rests at a grace";
			}
			if (state::Parked()) {
				return "Tarnished mode";
			}
			return nullptr;
		};
		if (cmd > 0 && ordersOn && !g_job.Busy()) {
			g_job.Start(cmd, a_nowMs);
			Log::Info("quiet: the player's order {} -- for the Elden Ring enemy the host picks", cmd);
		} else if (cmd > 0 && !g_job.Busy()) {
			Log::Info("quiet: the player's order {} with her orders off -- nothing", cmd);
		}
		if (g_job.Busy()) {
			const auto        ded = proxy::DedicatedState();
			const auto*       rec = findRecord(g_job.Handle());
			plan::JobInput    in;
			in.nowMs = a_nowMs;
			in.pick = pick ? pick : (g_pickSeen && a_nowMs - g_pickSeenMs <= 1000 ? g_pickSeen : 0);
			static int absent = 0;
			absent = rec ? 0 : absent + 1;
			in.targetPresent = !(rec && (rec->flags & proto::kActorDead)) && !(rec && !(rec->flags & proto::kActorHostile)) && absent < 3;
			in.proxyReady = ded.handle == g_job.Handle() && ded.ready && ded.soldier != 0xFFFF;
			if (in.proxyReady) {
				g_jobSoldier = ded.soldier;
			}
			{
				// Her rifle's hit only (290-292, or another id while her equipment var names a rifle): her dash attacks (141 / 142, the same
				// message) on the stand-in are no order's round (review: they reached the host as a lethal "unknown").
				std::scoped_lock lock(g_hitLock);
				const bool mine = g_job.OrderedMs() && g_lastHit.ms >= g_job.OrderedMs() && g_lastHit.target == g_jobSoldier;
				in.hit = mine && plan::OrderRound(g_lastHit.attack, Weapon());
				static std::uint64_t ignoredMs = 0;
				if (mine && !in.hit && g_lastHit.ms != ignoredMs) {
					ignoredMs = g_lastHit.ms;
					Log::Info("quiet: her attack {} on the order's stand-in -- {}, ignored", g_lastHit.attack,
						g_lastHit.attack == 141 || g_lastHit.attack == 142 ? "her dash, not a round" : "no rifle known");
				}
			}
			// Her "can't" (kQuietNoTarget) since the order: MGSV won't take the shot -- the job ends Lost (MGSV) now, not after
			// HitTimeoutMs / AimHoldMs (review: a Take aim held 20 s and dropped the player's next orders meanwhile).
			in.cant = g_job.OrderedMs() != 0 && g_cantMs.load(std::memory_order_relaxed) >= g_job.OrderedMs();
			const char* why = cancel();
			in.cancel = why != nullptr || !ordersOn;
			const auto step = g_job.Update(in, { .pickWaitMs = q.pickWaitMs, .realizeTimeoutMs = q.realizeTimeoutMs, .hitTimeoutMs = q.hitTimeoutMs, .aimHoldMs = q.aimHoldMs });
			const auto handle = g_job.Handle();
			if (step.dedicate) {
				proxy::Dedicate(handle, true, state::kBuddyTypeQuiet, q.standInHeight == QuietHeight::kGround ? proxy::plan::Height::kGround : proxy::plan::Height::kEnemy);
				g_standIn.store(true);
				g_standInHandle = handle;
				g_jobSoldier = 0xFFFF;
				Log::Info("quiet: order {} for Elden Ring {:#x}: her stand-in asked for", g_job.Cmd(), handle);
			}
			if (step.order) {
				const bool sent = follow::SendCommand(g_job.Cmd(), g_jobSoldier, "quiet: her order");
				// The safeguard: her kept aim target (R+0x186) and the requested one (R+0x182) are his, before the 1.8 s delay runs out.
				std::string writes;
				if (const auto r = Record()) {
					const auto aim = Get<std::uint16_t>(r + plan::kRecTargetAim);
					const auto req = Get<std::uint16_t>(r + plan::kRecTargetRequested);
					if (aim && *aim != g_jobSoldier) {
						const std::uint16_t id = g_jobSoldier;
						writes = std::format("; R+0x186 {:#x} -> {:#x} {}, R+0x182 {:#x} -> {:#x} {}", *aim, id, Put(r + plan::kRecTargetAim, &id, 2) ? "written" : "NOT written",
							req.value_or(0xFFFF), id, Put(r + plan::kRecTargetRequested, &id, 2) ? "written" : "NOT written");
					}
				}
				Log::Info("quiet: order {} for Elden Ring {:#x} -> stand-in {:#x} ready in {} ms -> command {} arg {:#x} {}{}", g_job.Cmd(), handle, g_jobSoldier,
					a_nowMs - g_job.OrderedMs(), g_job.Cmd(), g_jobSoldier, sent ? "sent" : "NOT sent", writes);
			}
			if (step.event) {
				std::uint32_t attack = 0, b = 0;
				if (*step.event == proto::kQuietHit) {
					std::scoped_lock lock(g_hitLock);
					attack = g_lastHit.attack;
				}
				if (*step.event == proto::kQuietHit) {
					g_weapon.store(plan::WeaponOfAttack(attack) != proto::kQuietWeaponUnknown ? plan::WeaponOfAttack(attack) : Weapon());
					b = ShareOf(attack);
				} else if (*step.event == proto::kQuietLost) {
					b = static_cast<std::uint32_t>(step.lost);
				}
				SendShot(*step.event, handle, attack, b,
					step.event == proto::kQuietLost ? (step.lost == proto::kQuietLostMgsv ? "no shot (MGSV)" : step.why) : step.why);
			}
			if (step.release) {
				const bool keep = !q.releaseAfterHit ? true
				                                     : step.event == proto::kQuietReleased && Weapon() != proto::kQuietTranq && pick == handle &&
				                                           plan::WantStandIn(pick, (lua.flags & state::kLBinocle) != 0, g_lastBinocMs, a_nowMs, q.keepMarkMs) && q.markProxy;
				if (!keep) {
					proxy::Release(step.why);
					g_standIn.store(false);
					g_standInHandle = 0;
				}
				g_jobSoldier = 0xFFFF;
			}
		}
		// MarkProxy: a stand-in while the host's pick holds and the binoculars are (or were, KeepMarkMs) up (§3.4).
		if (ordersOn && q.markProxy && !g_job.Busy()) {
			const bool wantIt = plan::WantStandIn(pick, (lua.flags & state::kLBinocle) != 0, g_lastBinocMs, a_nowMs, q.keepMarkMs);
			if (wantIt && (!g_standIn.load() || g_standInHandle != pick)) {
				proxy::Dedicate(pick, true, state::kBuddyTypeQuiet, q.standInHeight == QuietHeight::kGround ? proxy::plan::Height::kGround : proxy::plan::Height::kEnemy);
				g_standIn.store(true);
				g_standInHandle = pick;
			} else if (!wantIt && g_standIn.load()) {
				proxy::Release(pick ? "the binoculars went down long ago" : "the host's pick went");
				g_standIn.store(false);
				g_standInHandle = 0;
			}
		} else if (!g_job.Busy() && g_standIn.load() && !g_standInConsole.load() && (!ordersOn || cancel())) {
			proxy::Release(cancel() ? cancel() : "her orders are off");
			g_standIn.store(false);
			g_standInHandle = 0;
		}
		// kBuddyQuietMarked: MGSV holds a mark on the stand-in -- the binoculars were up within 0.5 s while he stood ready (and no wipe
		// runs while he is out: KeepMarks). Kept for KeepMarkMs.
		{
			// (live M1) MGSV's own verdict: its call menu's Fire is live for her -- she snipes from one of MGSV's snipe points with the
			// stand-in as her target. Only then does an order through MGSV's menu exist at all.
			const auto ded = proxy::DedicatedState();
			const bool ready = g_standIn.load() && ded.ready && ded.soldier != 0xFFFF;
			const auto live = ready ? FireLive() : std::nullopt;
			g_marked.store(ready && live.value_or(false));
			// Her sniping position: MGSV's Quiet takes her shot only from a snipe point of MGSV's (her standby beside Snake, and the mirror,
			// leave Fire greyed and a Fire there ends in her "can't", live M1). Once the stand-in is ready and Fire isn't live within 1.5 s,
			// she is asked to take up a sniping position (the block controller's move order: she dashes there cloaked, ~10-20 s).
			static std::uint64_t readySince = 0;
			static bool          snipeAsked = false, seenLive = false;
			if (!g_standIn.load()) {
				readySince = 0;
				snipeAsked = false;
				seenLive = false;
			} else if (live.value_or(false)) {
				seenLive = true;
			}
			if (ready && !readySince) {
				readySince = a_nowMs;
			}
			if (ready && !snipeAsked && !seenLive && !g_job.Busy() && readySince && a_nowMs - readySince > 1500 && layer) {
				snipeAsked = true;
				const auto fr = state::Frame();
				const auto r = a_call(std::format(
					"local ok, r = pcall(TppBuddy2BlockController.RequestMoveToPosition, BuddyType.QUIET, Vector3({:.2f}, {:.2f}, {:.2f}), 0) return tostring(ok) .. ' ' .. tostring(r)",
					fr.root.x, fr.root.y, fr.root.z));
				Log::Info("quiet: her stand-in {:#x} is ready but MGSV's Fire isn't live -- asked to take up a sniping position: {}", ded.soldier, r);
			}
		}
		g_jobBusy.store(g_job.Busy());
		g_engaged.store(g_job.Engaged());
		if (g_job.Busy() || g_standIn.load()) {
			g_markersUntil.store(a_nowMs + 1500);
		}
		capture::hud::SetHideQuietMarkers(q.hideMgsvMarkers && a_nowMs < g_markersUntil.load());
		// Cover me found on with [Quiet] Cover off: Cease cover (5) sent by the guest.
		if (layer && !q.cover && g_cover.load()) {
			static std::uint64_t ceaseAt = 0;
			if (a_nowMs - ceaseAt > 3000) {
				ceaseAt = a_nowMs;
				Log::Info("quiet: MGSV's Cover me is on with [Quiet] Cover off -- Cease cover sent: {}", follow::SendCommand(plan::kCmdCease, 0xFFFF, "quiet: cover off") ? "ok" : "refused");
			}
		}
	}

	bool KeepMarkers()
	{
		const int f = g_keepMarksForce.load();
		if (f >= 0) {
			return f == 1;
		}
		return Cfg().quiet.keepMarks && g_standIn.load();
	}

	bool OnPlayerCommand(int a_command, std::uint16_t a_arg)
	{
		if (g_sites != 1 || !plan::IsQuietCommand(a_command) || !QuietOut(state::Lua())) {
			return false;
		}
		const auto& q = Cfg().quiet;
		const plan::TakeOver t{ .layer = LayerOn() && HostLayer(), .orders = q.orders && !g_ordersOff.load(), .cover = q.cover, .talk = q.takeOverTalk };
		switch (plan::UseOf(a_command, t)) {
		case plan::CommandUse::kJob:
			if (g_jobBusy.load()) {
				Log::Info("quiet: the player's order {} (arg {:#x}) while an order runs -- ignored", a_command, a_arg);
				return true;
			}
			g_playerOrder.store(a_command);
			g_taken.fetch_add(1, std::memory_order_relaxed);
			Log::Info("quiet: the player's order {} (MGSV's own target arg {:#x}) taken over -- for the Elden Ring enemy the host picks", a_command, a_arg);
			return true;
		case plan::CommandUse::kRefuse:
			g_refused.fetch_add(1, std::memory_order_relaxed);
			Log::Info("quiet: the player's command {} taken over and not passed to MGSV ({})", a_command,
				a_command == plan::kCmdCover ? "Cover me: not in Elden Ring yet" : "the talk would turn MGSV's camera to her MGSV body");
			return true;
		default:
			g_passed.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
	}

	std::uint32_t OrderWeaponPad(int a_command)
	{
		if (!plan::IsOrderCommand(a_command)) {
			return 0;
		}
		const auto w = Weapon();
		return w == proto::kQuietWeaponUnknown ? 0u : static_cast<std::uint32_t>(w) + 1u;
	}

	std::uint32_t BuddyBits()
	{
		if (g_sites != 1) {
			return 0;
		}
		std::uint32_t b = proto::BuddyQuietWeaponBits(Weapon());
		const auto    now = ::GetTickCount64();
		if (now < g_cloakUntil.load() || g_away.load()) {
			b |= proto::kBuddyCloaked;
		}
		if (g_graceEcho.load()) {
			b |= proto::kBuddySeated;
		}
		if (g_atPost.load()) {
			b |= proto::kBuddyAtPost;
		}
		if (g_cover.load()) {
			b |= proto::kBuddyQuietCover;
		}
		if (LayerOn() && Cfg().quiet.restAtGrace) {
			b |= proto::kBuddyCanRest;
		}
		if (g_marked.load()) {
			b |= proto::kBuddyQuietMarked;
		}
		if (g_engaged.load()) {
			b |= proto::kBuddyEngaged;
		}
		return b;
	}

	std::uint32_t PostSeqEcho() { return g_postEcho.load(); }
	std::uint32_t GraceSeqEcho() { return g_graceEcho.load(); }

	std::optional<bool> KeepBuddyDraw(bool a_skinned, std::uint64_t a_meshKey, const float a_origin[3])
	{
		if (g_sites != 1 || !LayerOn() || !HostLayer()) {
			return std::nullopt;
		}
		const float x = g_rootX.load(), y = g_rootY.load(), z = g_rootZ.load();
		if (!std::isfinite(x)) {
			return std::nullopt;
		}
		const float d = std::sqrt((a_origin[0] - x) * (a_origin[0] - x) + (a_origin[1] - y) * (a_origin[1] - y) + (a_origin[2] - z) * (a_origin[2] - z));
		bool        keep = a_skinned ? d <= g_skinnedRadius.load() : d <= Cfg().quiet.rigidRadius;
		if (keep && !a_skinned && a_meshKey) {
			// A rigid draw within RigidRadius of her: hers once its mesh moved with her (review: a crate or a rail beside a held post was
			// published as hers); before that only while she is near Snake on the apron (her rifle the moment she is deployed).
			const float sx = g_snakeX.load(std::memory_order_relaxed), sz = g_snakeZ.load(std::memory_order_relaxed);
			const bool  nearSnake = std::isfinite(sx) && std::hypot(x - sx, z - sz) <= kKeyNearSnake;
			std::scoped_lock lock(g_keyLock);
			auto it = std::find_if(g_rigidKeys.begin(), g_rigidKeys.end(), [&](const RigidKey& k) { return k.key == a_meshKey; });
			if (it == g_rigidKeys.end() && g_rigidKeys.size() < 64) {
				g_rigidKeys.push_back({ a_meshKey, x, z, false });
				it = g_rigidKeys.end() - 1;
			}
			if (it != g_rigidKeys.end() && !it->confirmed && std::hypot(x - it->x, z - it->z) >= kKeyMoved) {
				it->confirmed = true;
				g_rigidConfirmed.fetch_add(1, std::memory_order_relaxed);
			}
			keep = (it != g_rigidKeys.end() && it->confirmed) || nearSnake;
		}
		(keep ? g_kept : g_propsDropped).fetch_add(1, std::memory_order_relaxed);
		return keep;
	}

	bool HideMarkers() { return Cfg().quiet.hideMgsvMarkers && ::GetTickCount64() < g_markersUntil.load(); }

	void DisableOrders(const std::string& a_why)
	{
		std::scoped_lock lock(g_textLock);
		if (!g_ordersOff.load()) {
			g_ordersOffWhy = a_why;
			g_ordersOff.store(true);
			Log::Warn("quiet: her orders OFF for this session -- {} (`!quiet orders on` tries again)", a_why);
		}
	}

	void TraceFrame(std::uint64_t a_frame, const Vec3& a_root, const float a_pub[3], std::uint32_t a_buddyFlags)
	{
		if (const int want = g_traceWant.exchange(0)) {
			if (want > 0 && !g_traceFile) {
				const auto path = ModuleDir() + L"PhantomRingQuietTrace.csv";
				g_traceFile = _wfopen(path.c_str(), L"ab");
				g_traceRows = 0;
				if (g_traceFile) {
					std::fputs("# frame,ms,root.x,root.y,root.z,k.x,k.z,lua.x,lua.y,lua.z,feet.x,feet.y,feet.z,skin.x,skin.y,skin.z,skinYaw,pub.x,pub.y,pub.z,bflags,"
					           "hold,hold.x,hold.z,holdAt,R.disabled,R.active,R.requested,R.t180,R.t182,R.t186,R0,hits,cants\n",
						g_traceFile);
				}
				Log::Info("quiet: trace {}", g_traceFile ? "on (PhantomRingQuietTrace.csv next to the DLL, one line per frame)" : "could NOT be opened");
			} else if (want < 0 && g_traceFile) {
				std::fclose(g_traceFile);
				g_traceFile = nullptr;
				Log::Info("quiet: trace off ({} lines)", g_traceRows);
			}
		}
		if (!g_traceFile) {
			return;
		}
		const auto lua = state::Lua();
		Vec3       skinAt;
		float      skinYaw = NAN;
		const bool skin = skeleton::BuddyPose(skinAt, skinYaw, 300);
		const auto rec = Record();
		const auto u32 = [&](std::uint32_t a_off) {
			const auto v = Get<std::uint32_t>(rec + a_off);
			return v ? std::format("{:#x}", *v) : std::string();
		};
		const auto u16 = [&](std::uint32_t a_off) {
			const auto v = Get<std::uint16_t>(rec + a_off);
			return v ? std::format("{:#x}", *v) : std::string();
		};
		const auto r0 = Get<std::uint64_t>(rec);
		const auto v3 = [](const std::optional<Vec3>& a_v) {
			return a_v ? std::format("{:.3f},{:.3f},{:.3f}", a_v->x, a_v->y, a_v->z) : std::string(",,");
		};
		const Vec3 hold = g_hold.odo + g_kNow;
		const auto line = std::format("{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:#x},{},{:.3f},{:.3f},{},{},{},{},{},{},{},{},{},{}\n", a_frame,
			::GetTickCount64(), a_root.x, a_root.y, a_root.z, g_kNow.x, g_kNow.z, v3(lua.buddyPosValid ? std::optional<Vec3>(lua.buddyPos) : std::nullopt), v3(g_feet),
			v3(skin ? std::optional<Vec3>(skinAt) : std::nullopt), skin ? skinYaw : NAN, a_pub[0], a_pub[1], a_pub[2], a_buddyFlags, static_cast<int>(g_hold.why),
			hold.x, hold.z, g_hold.at ? 1 : 0, u32(plan::kRecDisabled), u32(plan::kRecActive), u32(plan::kRecRequested), u16(plan::kRecTargetActive),
			u16(plan::kRecTargetRequested), u16(plan::kRecTargetAim), r0 ? std::format("{:#x}", *r0) : std::string(), g_hits.load(), g_cants.load());
		std::fputs(line.c_str(), g_traceFile);
		if (++g_traceRows % 60 == 0) {
			std::fflush(g_traceFile);
		}
		if (g_traceRows >= 400000) {
			std::fclose(g_traceFile);
			g_traceFile = nullptr;
		}
	}

	// ---- console ---------------------------------------------------------------------------------------------------------------
	std::string Command(const std::string& a_args)
	{
		const auto lua = state::Lua();
		const auto fr = state::Frame();
		const auto words = [&] {
			std::vector<std::string> w;
			std::size_t              at = 0;
			while (at < a_args.size()) {
				const auto sp = a_args.find(' ', at);
				const auto end = sp == std::string::npos ? a_args.size() : sp;
				if (end > at) {
					w.emplace_back(a_args.substr(at, end - at));
				}
				at = end + 1;
			}
			return w;
		}();
		const auto w0 = words.empty() ? std::string() : words[0];
		const auto w1 = words.size() > 1 ? words[1] : std::string();
		if (w0.empty() || w0 == "status") {
			return Describe();
		}
		if (w0 == "r") {
			// Her order record, raw: R[0], the targets, the bits, the delay.
			const auto rec = Record();
			if (!rec) {
				return "quiet: her order record isn't known (no message seen yet)";
			}
			std::string s = std::format("quiet: R {:#x} (sys {:#x} index {}):", rec, g_sys.load(), g_index.load());
			for (std::uint32_t off = 0; off < 0x200; off += 8) {
				if (off % 0x40 == 0) {
					s += std::format("\n  +{:03x}", off);
				}
				const auto v = Get<std::uint64_t>(rec + off);
				s += v ? std::format(" {:016x}", *v) : std::string(" ????????????????");
			}
			const auto prov = [&]() -> std::string {
				const auto c = Get<std::uintptr_t>(g_sys.load() + 0x1E0);
				const auto e8 = c && *c ? Get<std::uintptr_t>(*c + 0xE8) : std::nullopt;
				const auto t = e8 && *e8 ? Get<std::uint16_t>(*e8 + 0x70) : std::nullopt;
				return t ? std::format("{:#x}", *t) : std::string("?");
			}();
			return s + std::format("\n  the target provider [[[sys+0x1E0]+0xE8]+0x70] = {}", prov);
		}
		if (w0 == "scan") {
			g_scanWanted = true;
			g_finder.RequestScan();
			return "quiet: her location object looked for (the finder runs while she is out)";
		}
		if (w0 == "shift") {
			float dx = 0, dz = 0;
			if (std::sscanf(a_args.c_str(), "shift %f %f", &dx, &dz) != 2 || !std::isfinite(dx) || !std::isfinite(dz) || std::fabs(dx) > 80 || std::fabs(dz) > 80) {
				return "usage: !quiet shift <dx> <dz>   (metres, at most 80 each)";
			}
			g_scanWanted = true;
			std::scoped_lock lock(g_holdLock);
			g_testShift = Vec3{ dx, 0.0f, dz };
			return std::format("quiet: shift by {:.2f},{:.2f} at the next frame", dx, dz);
		}
		if (w0 == "hold") {
			if (w1 == "off") {
				std::scoped_lock lock(g_holdLock);
				g_consoleHoldOff = true;
				return "quiet: the console's hold is off";
			}
			float x = 0, z = 0, yawDeg = NAN;
			Holder h;
			h.why = HoldWhy::kConsole;
			h.tolerance = 0.5f;
			const Vec3 k = g_kNow;
			if (w1 == "rel") {
				if (std::sscanf(a_args.c_str(), "hold rel %f %f %f", &x, &z, &yawDeg) < 2) {
					return "usage: !quiet hold rel <dx> <dz> [yaw_deg]";
				}
				const Vec3 at{ fr.root.x + x, fr.root.y, fr.root.z + z };
				h.odo = at - k;
				h.yaw = std::isfinite(yawDeg) ? yawDeg * std::numbers::pi_v<float> / 180.0f : std::atan2(-x, -z);  // facing Snake by default
			} else {
				if (std::sscanf(a_args.c_str(), "hold %f %f %f", &x, &z, &yawDeg) < 2) {
					return "usage: !quiet hold <x> <z> [yaw_deg] | rel <dx> <dz> [yaw_deg] | off   (MGSV world; Snake's y)";
				}
				const Vec3 at{ x, fr.root.y, z };
				h.odo = at - k;
				h.yaw = std::isfinite(yawDeg) ? yawDeg * std::numbers::pi_v<float> / 180.0f : std::atan2(fr.root.x - x, fr.root.z - z);
			}
			g_scanWanted = true;
			static std::uint32_t consoleSeq = 0;
			h.seq = ++consoleSeq;  // each command a fresh hold (its own tries)
			std::scoped_lock lock(g_holdLock);
			g_consoleHold = h;
			const Vec3 at = h.odo + k;
			return std::format("quiet: held at {:.2f},{:.2f},{:.2f} facing {:.0f} deg ({:.1f} m from Snake) until `!quiet hold off`", at.x, at.y, at.z,
				h.yaw * 180.0f / std::numbers::pi_v<float>, (at - fr.root).LengthXZ());
		}
		if (w0 == "where") {
			Vec3        skinAt;
			float       yaw = NAN;
			const bool  skin = skeleton::BuddyPose(skinAt, yaw, 500);
			const auto  loc = g_finder.Confirmed();
			const auto  feet = loc ? buddyloc::Finder::Feet(loc) : std::nullopt;
			const auto  rot = loc ? buddyloc::Finder::Rotation(loc) : std::nullopt;
			const float rotYaw = rot ? logic::QuatYaw(rot->data()) : NAN;
			Snapshot snap;
			{
				std::scoped_lock lock(g_holdLock);
				snap = g_snap;
			}
			auto        s = std::format("quiet: GetPosition {} | skin {} yaw {:.1f} deg | location object {:#x} ({}) feet {} yaw {:.1f} deg | Snake {} ({:.2f} m) | hold {} {}",
				lua.buddyPosValid ? V3(lua.buddyPos) : std::string("-"), skin ? V3(skinAt) : std::string("-"), yaw * 180.0f / std::numbers::pi_v<float>, loc,
				snap.confirmed ? "confirmed" : snap.candidate ? "candidate" : "none", V3(feet), rotYaw * 180.0f / std::numbers::pi_v<float>, V3(fr.root),
				lua.buddyPosValid ? (lua.buddyPos - fr.root).LengthXZ() : -1.0f, static_cast<int>(snap.holdWhy), V3(snap.holdAt));
			Log::Info("{}", s);
			return s;
		}
		if (w0 == "borrow") {
			if (w1 == "off") {
				g_borrowAsk.store(2);
				return "quiet: put back at the next Lua tick (see the log, or `!quiet` for the diff)";
			}
			if (w1 == "test-unguarded" || w1 == "unguarded") {
				g_borrowUnguardedTest.store(words.size() < 3 || words[2] != "off");
				return std::format("quiet: the borrow's own guard check {} (the real save guards stay as they are)",
					g_borrowUnguardedTest.load() ? "made to FAIL" : "back to normal");
			}
			g_borrowAsk.store(1);
			return "quiet: borrow at the next Lua tick (memory only; refused unless the save guards are complete)";
		}
		if (w0 == "bits") {
			g_borrowAsk.store(3);
			return "quiet: her save state read at the next Lua tick (the log)";
		}
		if (w0 == "diff") {
			g_borrowAsk.store(4);
			return "quiet: the raw diff against the borrow's snapshot at the next Lua tick (the log)";
		}
		if (w0 == "last") {
			std::scoped_lock lock(g_luaLock);
			return g_luaLast;
		}
		if (w0 == "lua") {
			// `!quiet lua <chunk>`: run on the Lua thread at the next step (her LuaStep), result in the log and `!quiet last`.
			std::scoped_lock lock(g_luaLock);
			g_luaQueue.push_back(a_args.substr(4));
			return "quiet: queued for the Lua thread";
		}
		if (w0 == "keepmarks") {
			g_keepMarksForce.store(w1 == "on" ? 1 : w1 == "off" ? 0 : -1);
			return std::format("quiet: the periodic marker wipe {}", g_keepMarksForce.load() == 1 ? "held off (console)" : g_keepMarksForce.load() == 0 ? "as configured (forced not held)" : "per [Quiet] KeepMarks");
		}
		if (w0 == "standin") {
			if (w1 == "off") {
				proxy::Release("console");
				g_standIn.store(false);
				g_standInConsole.store(false);
				g_standInHandle = 0;
				return "quiet: stand-in released";
			}
			const auto h = std::strtoull(w1.c_str(), nullptr, 0);
			const bool ground = words.size() > 2 && words[2] == "ground";
			if (!h) {
				return "usage: !quiet standin <0xhandle> [ground] | off";
			}
			proxy::Dedicate(h, true, state::kBuddyTypeQuiet, ground ? proxy::plan::Height::kGround : proxy::plan::Height::kEnemy);
			g_standIn.store(true);
			g_standInConsole.store(true);
			g_standInHandle = h;
			return std::format("quiet: Elden Ring {:#x} gets her stand-in ({})", h, ground ? "on the ground" : "at his height");
		}
		if (w0 == "cmd") {
			// `!quiet cmd <1-7> [arg]`: her buddy command natively (as the job sends it; not the player's: no hold, no kGEvBuddyOrder).
			const int  c = std::atoi(w1.c_str());
			const auto arg = words.size() > 2 ? static_cast<std::uint16_t>(std::strtoul(words[2].c_str(), nullptr, 0)) : std::uint16_t(0xFFFF);
			if (!plan::IsQuietCommand(c)) {
				return "usage: !quiet cmd <1-7> [arg]   (1/2 Fire, 3 Take aim, 4 Cover me, 5 Cease cover, 6 Shoot this, 7 talk)";
			}
			return std::format("quiet: command {} arg {:#x} {}", c, arg, follow::SendCommand(c, arg, "quiet console") ? "sent" : "NOT sent (follow's checks)");
		}
		if (w0 == "menufire") {
			g_menuFire.store(w1 == "on" ? 1 : w1 == "off" ? 0 : -1);
			return std::format("quiet: the call menu's Fire / Take aim {}", g_menuFire.load() == 1 ? "made live while her stand-in is marked (console)" : g_menuFire.load() == 0 ? "left to MGSV" : "per [Quiet] Orders");
		}
		if (w0 == "trace") {
			g_traceWant.store(w1 == "off" ? -1 : 1);
			return std::format("quiet: trace {} at the next frame", w1 == "off" ? "off" : "on");
		}
		if (w0 == "hits") {
			std::scoped_lock lock(g_hitLock);
			std::string      s = std::format("quiet: {} hits, {} \"can't\":", g_hits.load(), g_cants.load());
			for (const auto& h : g_hitLog) {
				s += std::format("\n  {} ms: attack {} on {:#x} (index {}, bit60 {})", h.ms, h.attack, h.target, h.index, h.flag60);
			}
			return s;
		}
		if (w0 == "orders") {
			if (w1 == "on") {
				g_ordersOff.store(false);
				MutableCfg().quiet.orders = true;
				proxy::RearmPhaseGuard("`!quiet orders on`");
			} else if (w1 == "off") {
				MutableCfg().quiet.orders = false;
			}
			return std::format("quiet: [Quiet] Orders = {} for this session{}", Cfg().quiet.orders, g_ordersOff.load() ? " (OFF by the phase lock)" : "");
		}
		if (w0 == "layer") {
			g_layerForce.store(w1 == "on" ? 1 : w1 == "off" ? 0 : -1);
			return std::format("quiet: the guest's Quiet layer {} (the host's kHostQuietLayer is {})", LayerOn() ? "on" : "off", HostLayer() ? "on" : "off");
		}
		if (w0 == "set") {
			// `!quiet set <key> <value>`: a [Quiet] switch for the session (tests).
			auto&       q = MutableCfg().quiet;
			const auto  v = words.size() > 2 ? words[2] : std::string();
			const bool  on = v == "on" || v == "true" || v == "1";
			if (w1 == "treadmill") q.treadmill = on;
			else if (w1 == "mirror") q.mirror = on;
			else if (w1 == "markproxy") q.markProxy = on;
			else if (w1 == "keepmarks") q.keepMarks = on;
			else if (w1 == "fireonlyonorders") q.fireOnlyOnOrders = on;
			else if (w1 == "cover") q.cover = on;
			else if (w1 == "talk") q.takeOverTalk = on;
			else if (w1 == "rest") q.restAtGrace = on;
			else if (w1 == "hidemarkers") q.hideMgsvMarkers = on;
			else if (w1 == "nodying") q.noDying = on;
			else if (w1 == "releaseafterhit") q.releaseAfterHit = on;
			else if (w1 == "standinheight") q.standInHeight = v == "ground" ? QuietHeight::kGround : QuietHeight::kEnemy;
			else if (w1 == "sitpose") q.sitPose = v == "kneel" ? QuietSitPose::kKneel : v == "seat" ? QuietSitPose::kSeat : QuietSitPose::kWatch;
			else if (w1 == "region") {
				std::vector<float> r;
				for (std::size_t i = 2; i < words.size(); ++i) {
					r.push_back(std::strtof(words[i].c_str(), nullptr));
				}
				if (r.size() == 1 || r.size() >= 8) {
					std::scoped_lock lock(g_holdLock);
					g_regionNew = r;  // the game thread takes it at its next frame (never a vector swapped under its feet)
				}
			} else if (w1 == "skinnedradius") g_skinnedRadius.store(std::strtof(v.c_str(), nullptr));
			else if (w1 == "tolerance") q.postTolerance = std::strtof(v.c_str(), nullptr);
			else return "usage: !quiet set treadmill|mirror|markproxy|keepmarks|fireonlyonorders|cover|talk|rest|hidemarkers|nodying|releaseafterhit <on|off> | "
				        "standinheight enemy|ground | sitpose kneel|watch|seat | region <r...> | skinnedradius <m> | tolerance <m>";
			return std::format("quiet: [Quiet] {} = {} for this session", w1, v);
		}
		return "usage: !quiet [status|r|scan|shift <dx> <dz>|hold <x> <z> [yaw]|hold rel <dx> <dz> [yaw]|hold off|where|borrow [off|test-unguarded [off]]|bits|diff|"
		       "last|lua <chunk>|keepmarks on|off|auto|standin <0xh> [ground]|off|trace on|off|hits|orders on|off|layer on|off|auto|set <key> <v>]";
	}

	std::string Describe()
	{
		const auto& q = Cfg().quiet;
		const auto  lua = state::Lua();
		const auto  ded = proxy::DedicatedState();
		std::string jobLast, offWhy;
		{
			std::scoped_lock lock(g_textLock);
			jobLast = g_jobLast;
			offWhy = g_ordersOffWhy;
		}
		std::string borrow, lastDiff;
		{
			std::scoped_lock bl(g_borrowLock);
			lastDiff = g_lastDiff;
			borrow = g_borrow.active ? std::format("BORROWED ({}, snapshot of {} bytes + static {:#04x}, re-applied {} times)", plan::StateName(g_borrow.state),
				                             g_borrow.snap.size(), g_borrow.staticSnap, g_borrow.reapplied)
			                         : "not borrowed";
		}
		Snapshot snap;
		{
			std::scoped_lock lock(g_holdLock);
			snap = g_snap;
		}
		const auto rec = Record();
		return std::format(
			"quiet: sites {} layer {} (the host's {}), out {}; system {:#x} index {} R {:#x}; messages {} ({} orders); hits {} (can't {}); weapon {} (equip var {}, live "
			"{}); cover {}\n"
			"  save state {} ({}); {}; last put-back diff: {}\n"
			"  location object {:#x} {} ({} scans), feet {}; holder {} at {} ({}); {} shifts ({} failed), {} placements ({} failed); last: {}\n"
			"  post echo {} grace echo {} (sit busy {}); cloaked {}; capture: {} draws kept, {} dropped as props, {} rigid meshes moved with her; borrow pending {}\n"
			"  orders {}{}: job {} on {:#x} (stand-in {:#x}), events ordered {} hit {} released {} refused {} lost {}; engaged {}; stand-in {} {:#x} (soldier {:#x}{}); "
			"marked {} (menu made live {} frames); keep markers {}; taken {}, refused {}, passed {}; last: {}",
			g_sites, LayerOn() ? "on" : "off", HostLayer() ? "on" : "off", QuietOut(lua), g_sys.load(), g_index.load(), rec, g_messages.load(), g_orderMessages.load(),
			g_hits.load(), g_cants.load(), plan::WeaponName(Weapon()), g_equip.load(), g_liveWeapon.load(), g_cover.load(), plan::StateName(plan::Classify(g_bits)),
			plan::DescribeBits(g_bits), borrow, lastDiff, snap.candidate, snap.confirmed ? "confirmed" : snap.candidate ? "candidate" : "none",
			snap.scans, V3(snap.feet), static_cast<int>(snap.holdWhy), V3(snap.holdAt), snap.holdThere ? "there" : "not there", g_shifts, g_shiftFails,
			g_places, g_placeFails, snap.lastPlace, g_postEcho.load(), g_graceEcho.load(), g_sit.Busy(), ::GetTickCount64() < g_cloakUntil.load(), g_kept.load(),
			g_propsDropped.load(), g_rigidConfirmed.load(), g_borrowPending, q.orders ? "on" : "off", g_ordersOff.load() ? std::format(" -- OFF this session: {}", offWhy) : std::string(),
			plan::StageName(g_job.GetStage()), g_job.Handle(), g_jobSoldier, g_phaseCounts[0], g_phaseCounts[1], g_phaseCounts[2], g_phaseCounts[3],
			g_phaseCounts[4], g_engaged.load(), g_standIn.load() ? "dedicated" : "none", g_standInHandle, ded.soldier, ded.ready ? " ready" : "", g_marked.load(),
			g_menuForced.load(), KeepMarkers(), g_taken.load(), g_refused.load(), g_passed.load(), jobLast);
	}
}
