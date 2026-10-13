#include "Follow.h"

#include "Addresses.h"
#include "Config.h"
#include "Death.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"
#include "State.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace phantom::guest::follow
{
	namespace
	{
		using BuddyCommandFn = void (*)(void* a_system, int a_command, std::uint16_t a_arg);
		using HandlerFn = std::int32_t* (*)(std::uint8_t* a_controller, std::int32_t* a_result, std::uint32_t a_index, std::uint64_t* a_message);

		int               g_ok{ -1 };  // -1 unchecked, 0 refused, 1 usable
		std::atomic<bool> g_enabled{ true };
		std::atomic<bool> g_requested{ false };
		std::atomic<bool> g_soon{ false };  // after a treadmill jump: whistle at the next Update if needed
		std::uint64_t     g_last{ 0 };
		std::uint64_t     g_whistles{ 0 };
		float             g_lastDist{ -1.0f };
		std::mutex        g_lock;
		std::string       g_why{ "not checked" };

		// The handler detour (game thread) and the frame/Lua threads share these under g_lock.
		HandlerFn         g_handler{ nullptr };
		bool              g_hooked{ false };
		// The player's own buddy commands (MGSV's call menu / iDroid: wait, search/assault mode, ...): any command but the
		// whistle pauses our whistles until the player whistles again, so a mode or "wait" the player chose sticks (live:
		// our whistles kept putting D-Walker back into assault mode). Our own commands are marked on the calling thread.
		BuddyCommandFn                g_command{ nullptr };
		bool                          g_commandHooked{ false };  // our detour sits on its first bytes (checked before hooking)
		thread_local bool             t_ourCommand{ false };
		std::atomic<bool>             g_playerHold{ false };
		std::atomic<int>              g_playerLast{ -1 };
		std::atomic<std::uint64_t>    g_playerCommands{ 0 };
		// How we send the walker ([Buddy] FollowMethod). MGSV's whistle (command 20) also puts D-Walker back into Normal
		// Mode when it is in another one, and the walker's mode watcher announces every mode change: that was the voice
		// the player kept hearing, and the mode they chose (Stealth = wait, Search, Intercept) never stuck.
		enum class Method
		{
			kCommand,  // MGSV's whistle command, as it is (resets the mode: a voice line when it wasn't Normal)
			kRestore,  // the same whistle; the handler detour keeps the walker's mode across it (live: Search Mode kept)
			kRecord,   // no whistle: the destination record written as the whistle writes it, mode untouched (live, default:
			           // a 70 m walker is still placed ~22 m away and walks over; 20 s run: median 4.7 m, max 10.3 m)
		};

		Method MethodOf(const std::string& a_name)
		{
			return a_name == "record" ? Method::kRecord : a_name == "restore" ? Method::kRestore : Method::kCommand;
		}

		const char* MethodName(Method a_m) { return a_m == Method::kRecord ? "record" : a_m == Method::kRestore ? "restore" : "command"; }

		// D-Walker's modes = record +0x5A (Addresses.h: command 21 + mode).
		constexpr const char* kModeNames[6] = { "normal", "mode1", "mode2", "stealth", "search", "intercept" };

		const char* ModeName(int a_mode) { return a_mode >= 0 && a_mode < 6 ? kModeNames[a_mode] : "unknown"; }

		// [Buddy] FollowModes: "normal search", "all", ... -> bit per mode; nothing recognised = Normal only.
		unsigned ModeMask(const std::string& a_list)
		{
			unsigned mask = 0;
			for (int m = 0; m < 6; ++m) {
				if (a_list.find(kModeNames[m]) != std::string::npos) {
					mask |= 1u << m;
				}
			}
			if (a_list.find("all") != std::string::npos) {
				mask = 0x3F;
			}
			return mask ? mask : 1u;
		}

		// Set from the ini at Install and by the console; read by the handler detour (game thread) and Update (Lua thread).
		std::atomic<Method>   g_method{ Method::kRecord };
		std::atomic<unsigned> g_modeMask{ 1u };

		bool g_modeSites{ false };     // the mode byte and the buddy walker's type are the ones analysed
		bool g_restoreSites{ false };  // ...and the whistle case's mode reset too
		// The buddy walker's controller (the handler's rcx for a GameObject of type 0x2C), learned from any message the
		// handler sees for it, and its vtable then: a controller whose vtable changed is not written to.
		std::atomic<std::uintptr_t> g_walkerCtl{ 0 };
		std::atomic<std::uintptr_t> g_walkerVt{ 0 };
		int                         g_lastMode{ -1 };
		std::uint64_t               g_orders{ 0 };          // record writes (FollowMethod = record)
		std::atomic<std::uint64_t>  g_modesKept{ 0 };       // whistles whose mode reset the detour undid
		std::uint64_t               g_modeSkips{ 0 };       // updates left alone because of the walker's mode
		std::atomic<bool>           g_fallbackLogged{ false };

		// Every voice line MGSV posts through its voice system (Addresses.h kVoicePost), counted and logged: D-Walker's mode
		// announcements come from its mode watcher (return kWalkerVoiceReturn). Auto-follow must cause none.
		using VoiceFn = std::uintptr_t (*)(void* a_system, std::uint64_t a_id, std::uint8_t a_flag);
		VoiceFn                    g_voice{ nullptr };
		std::atomic<std::uint64_t> g_voices{ 0 }, g_walkerVoices{ 0 };
		std::atomic<std::uint64_t> g_lastVoice{ 0 };

		std::uintptr_t VoiceDetour(void* a_system, std::uint64_t a_id, std::uint8_t a_flag)
		{
			const auto from = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
			const bool walker = from == addr::kWalkerVoiceReturn;
			const auto n = g_voices.fetch_add(1, std::memory_order_relaxed);
			if (walker) {
				g_walkerVoices.fetch_add(1, std::memory_order_relaxed);
			}
			g_lastVoice.store(a_id, std::memory_order_relaxed);
			if (n < 200) {
				Log::Info("voice: MGSV says {:#x}{} (from {:#x})", a_id, walker ? " -- D-Walker's mode announcement" : "", from);
			}
			return g_voice(a_system, a_id, a_flag);
		}

		std::atomic<int>  g_ours{ 0 };            // whistles we issued that the handler hasn't seen yet
		std::uintptr_t    g_record{ 0 };          // the walker record our last retarget wrote
		logic::Vec3       g_spot;                 // ...and the spot it holds now
		std::uint64_t     g_retargets{ 0 };
		std::uint64_t     g_shifts{ 0 };
		std::uint64_t     g_steers{ 0 };
		bool              g_moving{ false };      // Snake was moving at the last Update
		std::uint64_t     g_stuckSince{ 0 };      // the walker has stood still since then
		logic::Vec3       g_buddyLast;
		bool              g_buddyLastValid{ false };

		bool ReadPtr(std::uintptr_t a_at, std::uintptr_t& a_out)
		{
			SIZE_T got = 0;
			return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(a_at), &a_out, sizeof(a_out), &got) && got == sizeof(a_out);
		}

		bool ReadBytes(std::uintptr_t a_at, void* a_out, std::size_t a_n)
		{
			SIZE_T got = 0;
			return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(a_at), a_out, a_n, &got) && got == a_n;
		}

		bool WriteBytes(std::uintptr_t a_at, const void* a_in, std::size_t a_n)
		{
			SIZE_T put = 0;
			return ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<void*>(a_at), a_in, a_n, &put) && put == a_n;
		}

		// The static buddy system must still be the object we analysed: its vtable, and the command slot pointing
		// at the function whose bytes (and command-20 case) we checked.
		bool Check()
		{
			std::uintptr_t vt = 0, fn = 0;
			std::string    why;
			// The prologue was checked before our detour replaced it (live: re-checking it here found our jump and turned
			// the follow off for the whole session); the whistle case further in is never patched.
			if ((!g_commandHooked && !Matches(addr::kBuddyCommand, "buddy command")) || !Matches(addr::kBuddyCommandWhistle, "buddy command: whistle case")) {
				why = "bytes differ";
			} else if (!ReadPtr(addr::kBuddySystem, vt) || vt != addr::kBuddySystemVtable) {
				why = std::format("buddy system vtable {:#x}, expected {:#x}", vt, addr::kBuddySystemVtable);
			} else if (!ReadPtr(vt + addr::kBuddyCommandSlot, fn) || fn != addr::kBuddyCommand.va) {
				why = std::format("command slot {:#x}, expected {:#x}", fn, addr::kBuddyCommand.va);
			}
			std::scoped_lock lock(g_lock);
			g_why = why.empty() ? "ready" : "disabled: " + why;
			Log::Info("follow: {}", g_why);
			return why.empty();
		}

		bool SpotWanted() { return Cfg().buddyFollowAhead != 0.0f || Cfg().buddyFollowSide != 0.0f; }

		// The walker's record (the handler's lookup: [[controller+0x60]+8] + index*0x60, index < [[controller+0x60]+0x10]).
		std::uintptr_t RecordOf(std::uintptr_t a_controller, std::uint32_t a_index)
		{
			std::uintptr_t table = 0, base = 0;
			std::uint32_t  count = 0;
			if (!a_controller || !ReadPtr(a_controller + 0x60, table) || !table || !ReadBytes(table + 0x10, &count, 4) || a_index >= count ||
				!ReadPtr(table + 8, base) || !base) {
				return 0;
			}
			return base + std::uintptr_t(a_index) * 0x60;
		}

		// The buddy walker's controller as learned, if it still looks like it (same vtable, type 0x2C).
		std::uintptr_t WalkerController()
		{
			const auto     ctl = g_walkerCtl.load(std::memory_order_acquire);
			std::uintptr_t vt = 0;
			std::uint16_t  type = 0;
			if (!ctl || !ReadPtr(ctl, vt) || vt != g_walkerVt.load(std::memory_order_acquire) || !ReadBytes(ctl + 8, &type, 2) ||
				type != addr::kWalkerBuddyType) {
				return 0;
			}
			return ctl;
		}

		// D-Walker's mode (0..5) for the buddy object a_id, -1 = not known (no controller yet, sites differ).
		int WalkerMode(std::uint16_t a_id)
		{
			std::uint8_t mode = 0xFF;
			const auto   record = g_modeSites ? RecordOf(WalkerController(), a_id & 0x1FF) : 0;
			if (!record || !ReadBytes(record + addr::kWalkerRecordMode, &mode, 1) || mode > 5) {
				return -1;
			}
			return mode;
		}

		// The handler's own `this` (it reads the type at +8 itself): plain reads, no syscall per message.
		void Learn(std::uint8_t* a_controller, std::uint64_t a_message)
		{
			const auto ctl = reinterpret_cast<std::uintptr_t>(a_controller);
			if (!g_modeSites || !a_controller || ctl == g_walkerCtl.load(std::memory_order_relaxed) ||
				*reinterpret_cast<const std::uint16_t*>(a_controller + 8) != addr::kWalkerBuddyType) {
				return;  // enemy walker gears share this handler; only the buddy's (type 0x2C) is ours
			}
			const auto vt = *reinterpret_cast<const std::uintptr_t*>(a_controller);
			g_walkerVt.store(vt, std::memory_order_release);
			g_walkerCtl.store(ctl, std::memory_order_release);
			Log::Info("follow: D-Walker's controller {:#x} (vtable {:#x}), learned from message {:#x}", ctl, vt, a_message);
		}

		// The spot is beside Snake *as the camera sees him*: ahead/right along the camera's view, not his facing. Beside
		// his facing, a Snake turned toward the camera has his left side between him and the camera (MGSV's own frame
		// showed the walker filling the right of the screen). The view from MGSV's camera to Snake, horizontally.
		float ViewYaw(const state::FrameView& a_fr, float a_fallback)
		{
			const float dx = a_fr.root.x - a_fr.camPos.x, dz = a_fr.root.z - a_fr.camPos.z;
			return a_fr.valid && dx * dx + dz * dz > 0.25f ? std::atan2(dx, dz) : a_fallback;
		}

		// A running Snake gets the walker further out to the side: MGSV's walker stops walking once it is within ~3 m of the
		// player (live: at 2.7 m beside a running Snake, then fell 8 m behind before the next whistle got it going again).
		bool Running(const state::FrameView& a_fr) { return a_fr.valid && a_fr.velocity.LengthXZ() > 2.0f; }

		float Side(bool a_running)
		{
			const float side = Cfg().buddyFollowSide;
			const float running = Cfg().buddyFollowSideRunning;
			return a_running && std::fabs(running) > std::fabs(side) ? running : side;
		}

		logic::Vec3 Spot(const logic::Vec3& a_snake, float a_yaw, bool a_running = false)
		{
			return logic::BesideOf(a_snake, a_yaw, Cfg().buddyFollowAhead, Side(a_running));
		}

		// Where Snake will be in FollowLeadSec (his smoothed velocity, horizontal, at most 8 m ahead): a walker sent to where he
		// is now arrives behind a running Snake and spends the run catching up (live: up to 14 m behind).
		logic::Vec3 Lead(const state::FrameView& a_fr)
		{
			return logic::LeadPoint(a_fr.root, a_fr.velocity, Cfg().buddyFollowLeadSec, 8.0f, 1.0f);
		}

		logic::Vec3 Target(const state::FrameView& a_fr) { return Spot(Lead(a_fr), ViewYaw(a_fr, a_fr.yaw), Running(a_fr)); }

		// Catch-up: a walker that fell far behind a running Snake is moved closer -- only from a place the camera doesn't see
		// to one it doesn't see either (behind the camera; MGSV's camera is Elden Ring's), so nobody sees it jump. MGSV's
		// walker needs ~3 s to answer a whistle (live: whistle -> order taken ~2 s -> walking ~1 s later) and walks ~6.5 m/s:
		// without this it trails up to ~19 m before it catches up with a 4.7 m/s run.
		std::uint64_t g_lastCatchUp{ 0 };
		std::uint64_t g_catchUps{ 0 };

		void CatchUp(const state::FrameView& a_fr, const state::LuaView& a_lua, std::uint64_t a_nowMs)
		{
			const float limit = Cfg().buddyFollowCatchUp;
			if (!(limit > 0.0f) || !Running(a_fr) || a_nowMs - g_lastCatchUp < 1500) {
				return;
			}
			const logic::Vec3 toSnake = a_fr.root - a_fr.camPos;
			const float       camBack = toSnake.LengthXZ();
			const float       gap = (a_lua.buddyPos - a_fr.root).LengthXZ();
			if (camBack < 0.5f || gap < limit) {
				return;
			}
			// In view: in front of the camera and inside its horizontal field (MGSV's ~60 deg; 35 deg half-angle plus the
			// walker's own size). MGSV's camera is Elden Ring's.
			const logic::Vec3 view{ toSnake.x / camBack, 0.0f, toSnake.z / camBack };
			const auto        inView = [&](const logic::Vec3& a_p) {
                const float dx = a_p.x - a_fr.camPos.x, dz = a_p.z - a_fr.camPos.z;
                const float depth = dx * view.x + dz * view.z;
                const float lateral = std::fabs(dx * view.z - dz * view.x);
                return depth > -1.0f && lateral < std::max(0.0f, depth) * 0.7f + 2.0f;
			};
			if (inView(a_lua.buddyPos)) {
				return;  // nobody may see it jump
			}
			// Where to: beside Snake and a little behind him on the side the spot is (out of the camera's field at that
			// angle), else behind the camera along his motion.
			const float       viewYaw = std::atan2(view.x, view.z);
			const logic::Vec3 motion = a_fr.velocity * (1.0f / std::max(0.1f, a_fr.velocity.LengthXZ()));
			logic::Vec3       to = logic::BesideOf(a_fr.root, viewYaw, -2.5f, Side(true));
			if (inView(to)) {
				const float back = std::max(limit, camBack + 1.5f);
				to = { a_fr.root.x - motion.x * back, a_fr.root.y, a_fr.root.z - motion.z * back };
			}
			if (inView(to) || (to - a_fr.root).LengthXZ() >= gap - 1.0f) {
				return;
			}
			const float yaw = std::atan2(motion.x, motion.z);
			state::QueueCommand(std::format("owarp {} {} {} {} {}", a_lua.buddyId, to.x, a_fr.root.y, to.z, yaw));
			g_lastCatchUp = a_nowMs;
			++g_catchUps;
			Log::Debug("follow: catch-up {:.1f} m -> {:.1f} m from Snake (out of view)", gap, (to - a_fr.root).LengthXZ());
		}

		// Steering (the walker is already walking to a spot we wrote): the record must still hold our spot.
		bool StillOurs()
		{
			if (!g_record) {
				return false;
			}
			float now[3]{};
			if (!ReadBytes(g_record + 0x30, now, sizeof(now)) || std::fabs(now[0] - g_spot.x) > 0.01f || std::fabs(now[2] - g_spot.z) > 0.01f) {
				g_record = 0;  // MGSV wrote something else there since (the player's own whistle, a new order): not ours to move
				return false;
			}
			return true;
		}

		bool MoveSpot(const logic::Vec3& a_to)
		{
			const float out[3] = { a_to.x, a_to.y, a_to.z };
			if (!WriteBytes(g_record + 0x30, out, sizeof(out))) {
				return false;
			}
			g_spot = a_to;
			return true;
		}

		// Right after the handler stored Snake's position and rotation in the walker's record: put the spot there
		// instead (the walker reads it when it walks, after this message).
		void Retarget(std::uint8_t* a_controller, std::uint32_t a_index)
		{
			const auto           c = reinterpret_cast<std::uintptr_t>(a_controller);
			std::uint8_t         flags = 0;
			const std::uintptr_t record = ReadBytes(c + 0xFC, &flags, 1) && !(flags & 2) ? RecordOf(c, a_index) : 0;
			if (!record) {
				return;  // the handler ignored this whistle, or the layout isn't the one analysed
			}
			float                pos[4]{}, rot[4]{};
			std::uint8_t         pending = 0;
			if (!ReadBytes(record + 0x30, pos, sizeof(pos)) || !ReadBytes(record + 0x40, rot, sizeof(rot)) || !ReadBytes(record + 0x5C, &pending, 1) || pending != 1) {
				return;
			}
			const logic::Vec3 at{ pos[0], pos[1], pos[2] };
			const float       norm = rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3];
			if (!at.Finite() || std::fabs(norm - 1.0f) > 0.05f) {
				return;
			}
			// The stored position must be Snake's (the handler reads the player's): else leave it to MGSV.
			auto fr = state::Frame();
			if (fr.valid && (at - fr.root).LengthXZ() > 3.0f) {
				return;
			}
			fr.root = at;
			const auto spot = fr.valid ? Spot(Lead(fr), ViewYaw(fr, logic::QuatYaw(rot)), Running(fr)) : Spot(at, logic::QuatYaw(rot));
			const float out[3] = { spot.x, spot.y, spot.z };
			if (!WriteBytes(record + 0x30, out, sizeof(out))) {
				return;
			}
			std::scoped_lock lock(g_lock);
			g_record = record;
			g_spot = spot;
			++g_retargets;
		}

		std::int32_t* HandlerDetour(std::uint8_t* a_controller, std::int32_t* a_result, std::uint32_t a_index, std::uint64_t* a_message)
		{
			std::uint64_t id = 0;
			if (a_message) {
				ReadBytes(reinterpret_cast<std::uintptr_t>(a_message), &id, sizeof(id));
			}
			Learn(a_controller, id);
			// Only whistles we issued: the player's own (LB call menu) keep MGSV's meaning, come to Snake (and Normal Mode).
			bool ours = false;
			if (id == addr::kWhistleMessage && g_ours.load(std::memory_order_acquire) > 0) {
				g_ours.fetch_sub(1, std::memory_order_acq_rel);
				ours = true;
			}
			// Keep the mode: the whistle case resets a non-zero mode byte to 0 (and sets the announcement's reason byte). With
			// the byte 0 while the handler runs it does neither; we put the mode back right after, in the same call on the same
			// thread, so the walker's mode watcher never sees a change and says nothing.
			std::uintptr_t record = 0;
			std::uint8_t   mode = 0;
			const auto     method = g_method.load(std::memory_order_relaxed);
			if (ours && g_restoreSites && method != Method::kCommand &&
				reinterpret_cast<std::uintptr_t>(a_controller) == g_walkerCtl.load(std::memory_order_acquire)) {
				record = RecordOf(reinterpret_cast<std::uintptr_t>(a_controller), a_index);
				const std::uint8_t zero = 0;
				if (!record || !ReadBytes(record + addr::kWalkerRecordMode, &mode, 1) || mode == 0 || mode > 5 ||
					!WriteBytes(record + addr::kWalkerRecordMode, &zero, 1)) {
					record = 0;
				}
			}
			auto* r = g_handler(a_controller, a_result, a_index, a_message);
			if (record) {
				WriteBytes(record + addr::kWalkerRecordMode, &mode, 1);
				g_modesKept.fetch_add(1, std::memory_order_relaxed);
				Log::Info("follow: our whistle kept D-Walker in {} mode", ModeName(mode));
			}
			if (ours && SpotWanted()) {
				Retarget(a_controller, a_index);
			}
			return r;
		}

		void CommandDetour(void* a_system, int a_command, std::uint16_t a_arg)
		{
			if (!t_ourCommand) {
				g_playerLast.store(a_command, std::memory_order_relaxed);
				g_playerCommands.fetch_add(1, std::memory_order_relaxed);
				// The whistle and "Normal Mode" (21: disengage from the current mode) both leave the walker in Normal Mode, the
				// mode our whistles don't change: following resumes. Any other command (a mode, another buddy's) holds it.
				const bool hold = a_command != addr::kBuddyWhistle && a_command != addr::kBuddyNormalMode && Cfg().buddyFollowRespectCommands;
				if (hold != g_playerHold.exchange(hold, std::memory_order_acq_rel)) {
					Log::Info("follow: the player's buddy command {} (arg {}) -> {}", a_command, a_arg,
						hold ? "auto-follow paused until the player whistles" : "auto-follow resumed");
				} else {
					Log::Info("follow: the player's buddy command {} (arg {})", a_command, a_arg);
				}
				if (hold) {
					std::scoped_lock lock(g_lock);
					g_record = 0;  // its destination is the player's now: no steering
				}
				// Tell the host (v7): Elden Ring shows the order, so the player knows it was given (players held LB and let go
				// without clicking the right stick, and never knew no order had gone out).
				proto::GuestEvent e{};
				e.type = proto::kGEvBuddyOrder;
				e.a = static_cast<std::uint32_t>(a_command);
				e.b = a_command >= 20 ? 4u : a_command >= 17 ? 1u : a_command >= 8 ? 2u : 3u;  // BuddyType by command range (18.3)
				link::PushEvent(e);
			}
			g_command(a_system, a_command, a_arg);
		}

		void Whistle(const char* a_why)
		{
			if (g_hooked) {
				g_ours.store(1, std::memory_order_release);  // one message per whistle; a stale one is overwritten
			}
			t_ourCommand = true;
			reinterpret_cast<BuddyCommandFn>(addr::kBuddyCommand.va)(reinterpret_cast<void*>(addr::kBuddySystem), addr::kBuddyWhistle, 0);
			t_ourCommand = false;
			++g_whistles;
			Log::Debug("follow: whistle #{} ({})", g_whistles, a_why);
		}

		// FollowMethod = record: what the whistle case does to the record, minus the mode reset -- destination at +0x30, the
		// facing at +0x40, the "new order" flag +0x5C last (the AI takes it on its next plan, ~2 s: a direct write of
		// +0x30/+0x5C from outside the process was taken the same way, GUEST.md 18.3). No message, no mode change, no voice.
		// False = the walker isn't known yet (no message for it seen): the caller whistles instead.
		bool SendRecord(std::uint16_t a_id, const logic::Vec3& a_spot, float a_yaw)
		{
			const auto     ctl = g_modeSites ? WalkerController() : 0;
			const auto     record = RecordOf(ctl, a_id & 0x1FF);
			std::uint8_t   flags = 0;
			if (!record || !ReadBytes(ctl + 0xFC, &flags, 1)) {
				return false;
			}
			if (flags & 2) {
				return true;  // the handler ignores whistles now (bit 1 of +0xFC): no order either
			}
			const float        pos[3] = { a_spot.x, a_spot.y, a_spot.z };
			const auto         rot = logic::YawQuat(a_yaw);
			const std::uint8_t pending = 1;
			if (!WriteBytes(record + 0x30, pos, sizeof(pos)) || !WriteBytes(record + 0x40, rot.data(), sizeof(float) * 4) ||
				!WriteBytes(record + addr::kWalkerRecordPending, &pending, 1)) {
				return false;
			}
			std::scoped_lock lock(g_lock);
			g_record = record;
			g_spot = a_spot;
			++g_orders;
			return true;
		}

		// One order to walk to a_spot, by the configured method.
		void Order(std::uint16_t a_id, const logic::Vec3& a_spot, float a_yaw, const std::string& a_why)
		{
			if (g_method.load(std::memory_order_relaxed) == Method::kRecord && g_hooked) {
				if (SendRecord(a_id, a_spot, a_yaw)) {
					Log::Debug("follow: order #{} by record ({})", g_orders, a_why);
					return;
				}
				if (!g_fallbackLogged.exchange(true)) {
					Log::Info("follow: D-Walker's record not known yet -- whistling (mode kept) until the handler has seen it");
				}
			}
			Whistle(a_why.c_str());
		}
	}

	void Install()
	{
		if (Cfg().buddy != "walker" || !Cfg().buddyFollow || !SpotWanted()) {
			return;
		}
		if (!Matches(addr::kWalkerMessageHandler, "walker message handler") || !Matches(addr::kWalkerWhistleRecord, "walker whistle record") ||
			!Matches(addr::kWalkerWhistleStore, "walker whistle store") || !Matches(addr::kWalkerWhistleFlag, "walker whistle flag")) {
			Log::Warn("follow: walker message handler differs -- whistles bring D-Walker to Snake himself (MGSV's own spot)");
			return;
		}
		// D-Walker's mode (record +0x5A, the buddy walker's type 0x2C): needed to leave the player's mode alone and for the
		// restore/record methods. Differing bytes = modes unknown, whistles as before (command method).
		g_modeSites = Matches(addr::kWalkerModeStore, "walker mode store") && Matches(addr::kWalkerModeOfCommand, "walker mode of command") &&
		              Matches(addr::kBuddyCommandMode, "buddy command: modes") && Matches(addr::kWalkerBuddyTypeCheck, "buddy walker type");
		g_restoreSites = g_modeSites && Matches(addr::kWalkerWhistleModeReset, "walker whistle mode reset");
		g_method = MethodOf(Cfg().buddyFollowMethod);
		g_modeMask = ModeMask(Cfg().buddyFollowModes);
		Log::Info("follow: method {} (modes {}), follow in modes '{}' (mask {:#x})", MethodName(g_method),
			g_restoreSites ? "known" : g_modeSites ? "known, whistle reset differs: restore off" : "unknown: command method only", Cfg().buddyFollowModes,
			g_modeMask.load());
		g_hooked = Hooks::Create("walker message handler", reinterpret_cast<void*>(addr::kWalkerMessageHandler.va), &HandlerDetour, &g_handler);
		if (Matches(addr::kBuddyCommand, "buddy command") &&
			Hooks::Create("buddy command", reinterpret_cast<void*>(addr::kBuddyCommand.va), &CommandDetour, &g_command)) {
			g_commandHooked = true;
			Log::Info("follow: the player's own buddy commands {}", Cfg().buddyFollowRespectCommands ? "pause auto-follow (until a whistle or Normal Mode)" : "are logged");
		}
		if (Matches(addr::kVoicePost, "voice post")) {
			Hooks::Create("voice post", reinterpret_cast<void*>(addr::kVoicePost.va), &VoiceDetour, &g_voice);
		}
		Log::Info("follow: {}", g_hooked ? std::format("whistles send D-Walker {:.1f} m ahead / {:.1f} m right of Snake", Cfg().buddyFollowAhead,
			                                               Cfg().buddyFollowSide)
		                                 : std::string("message handler hook refused -- whistles bring D-Walker to Snake himself"));
	}

	void Update(std::uint64_t a_nowMs, bool a_ready)
	{
		const auto& c = Cfg();
		if (!a_ready || c.buddy != "walker") {
			return;
		}
		if (g_ok < 0) {
			g_ok = Check() ? 1 : 0;
		}
		if (g_ok != 1) {
			return;
		}
		const auto lua = state::Lua();
		const auto fr = state::Frame();
		if (!lua.InGame() || !fr.valid || lua.buddyId == 0xFFFF || !lua.buddyPosValid) {
			return;
		}
		// The active buddy may not be the walker (it is missing from this save and the sortie buddy -- D-Dog,
		// the horse, Quiet -- is out): whistling would order that one about, and the armed retarget would
		// wait for a walker message that never comes, so drop it.
		if (lua.buddyActive != state::kBuddyTypeWalker || state::BuddyMissing()) {
			g_ours.store(0, std::memory_order_release);
			return;
		}
		// Only D-Walker: the active buddy's object must be the buddy walker (GameObject type 0x2C); the whistle means nothing
		// to another buddy and the record lookup below is the walker's.
		if ((lua.buddyId >> 9) != addr::kWalkerBuddyType) {
			return;
		}
		// The mode the player (or MGSV) put it in: auto-follow only acts in the FollowModes ones, so a Stealth (wait),
		// Search or Intercept Mode the player chose stays. -1 = not known yet: the player-command hold below decides alone.
		const int mode = WalkerMode(lua.buddyId);
		if (mode != g_lastMode) {
			Log::Info("follow: D-Walker mode {} -> {}", ModeName(g_lastMode), ModeName(mode));
			g_lastMode = mode;
		}
		const bool modeOurs = mode < 0 || ((g_modeMask.load(std::memory_order_relaxed) >> mode) & 1) != 0;
		if (g_requested.exchange(false)) {
			g_last = a_nowMs;
			Whistle("console");
			return;
		}
		// Where it should be: the follow spot (ahead of a moving Snake) when the handler hook places it there, else Snake.
		const bool spotted = g_hooked && SpotWanted();
		const auto spot = spotted ? Target(fr) : fr.root;
		const float dist = (lua.buddyPos - spot).LengthXZ();
		const bool  riding = lua.ridden != 0xFFFF || (lua.flags & state::kLVehicle);
		const bool  busy = (lua.flags & (state::kLMenu | state::kLDemo | state::kLDead | state::kLCqc)) != 0 || death::Down();
		const bool  soon = g_soon.exchange(false);
		const bool  moving = fr.velocity.LengthXZ() > 1.0f;
		// Snake started or stopped: a new order at once. MGSV's walker only takes a new destination when it plans (an
		// order, a path segment): steered spots alone left it walking on 9 m past a Snake who had stopped (live).
		const bool  started = moving != g_moving;
		g_moving = moving;
		// Is it walking? A walker that stopped (arrived, or MGSV finished its order) no longer reads the spot. Its own motion,
		// not the distance: running at Snake's pace it gains nothing and a new whistle would only stop and restart it.
		// A treadmill jump moves it by metres in one update: not walking, not standing either.
		const float step = g_buddyLastValid ? (lua.buddyPos - g_buddyLast).LengthXZ() : 0.0f;
		g_buddyLast = lua.buddyPos;
		g_buddyLastValid = true;
		if (step > 0.25f && step < 3.0f) {
			g_stuckSince = a_nowMs;  // walking
		} else if (step <= 0.25f && !g_stuckSince) {
			g_stuckSince = a_nowMs;
		}
		if (dist < c.buddyFollowDistance) {
			g_stuckSince = a_nowMs;
		}
		g_lastDist = dist;
		if (!g_enabled || !c.buddyFollow || riding || busy || g_playerHold.load(std::memory_order_acquire)) {
			return;
		}
		if (!modeOurs) {
			++g_modeSkips;
			std::scoped_lock lock(g_lock);
			g_record = 0;  // its own mode drives it now: no steering, no catch-up
			return;
		}
		if (spotted) {
			CatchUp(fr, lua, a_nowMs);
		}
		// Steering: while it walks to our spot, keep that spot where Snake is going (no new whistle, no stop and restart).
		{
			std::scoped_lock lock(g_lock);
			if (spotted && c.buddyFollowSteer && StillOurs() && (spot - g_spot).LengthXZ() > 0.3f && MoveSpot(spot)) {
				++g_steers;
			}
		}
		if (dist < c.buddyFollowDistance) {
			return;
		}
		// A new whistle: as soon as Snake starts moving; when the walker stood still for a second although it is far from its
		// spot; without steering (no spot of ours in the record) after a treadmill jump or every FollowEveryMs as before.
		// While it walks to a spot we steer, never: each whistle makes it stop, turn and start again (live: ~2 s lost each).
		const bool steerable = spotted && c.buddyFollowSteer && g_record;
		// MGSV's walker doesn't come closer than ~3 m to the player: standing that close to an idle Snake it has arrived.
		const bool besideIdle = !moving && (lua.buddyPos - fr.root).LengthXZ() < 3.5f;
		const bool stalled = a_nowMs - g_stuckSince > 1000 && !besideIdle;
		const bool due = a_nowMs - g_last >= static_cast<std::uint64_t>(c.buddyFollowEveryMs);
		if (!(started || (steerable ? stalled && due : (soon || due) && !besideIdle))) {
			return;
		}
		g_last = a_nowMs;
		g_stuckSince = a_nowMs;
		Order(lua.buddyId, spot, fr.yaw,
			std::format("{:.1f} m from its spot{}", dist, started ? (moving ? ", Snake started moving" : ", Snake stopped") : steerable ? ", standing still" : soon ? ", after a treadmill jump" : ""));
	}

	void OnShift(const logic::Vec3& a_shift)
	{
		g_soon = true;
		std::scoped_lock lock(g_lock);
		if (!g_record) {
			return;
		}
		// The walker was moved by the same jump: its destination must move too, or it walks back to where Snake was.
		float now[3]{};
		if (!ReadBytes(g_record + 0x30, now, sizeof(now)) || std::fabs(now[0] - g_spot.x) > 0.01f || std::fabs(now[2] - g_spot.z) > 0.01f) {
			g_record = 0;  // MGSV wrote something else there since: not ours to move any more
			return;
		}
		g_spot = g_spot + a_shift;
		const float out[3] = { g_spot.x, g_spot.y, g_spot.z };
		if (WriteBytes(g_record + 0x30, out, sizeof(out))) {
			++g_shifts;
		}
	}

	bool SetMethod(const std::string& a_name)
	{
		if (a_name != "command" && a_name != "restore" && a_name != "record") {
			return false;
		}
		g_method = MethodOf(a_name);
		g_fallbackLogged = false;
		Log::Info("follow: method {} (console)", a_name);
		return true;
	}

	void SetModes(const std::string& a_list)
	{
		g_modeMask = ModeMask(a_list);
		Log::Info("follow: follow in modes '{}' (mask {:#x}, console)", a_list, g_modeMask.load());
	}

	void RequestWhistle() { g_requested = true; }
	void SetEnabled(bool a_on) { g_enabled = a_on; }

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("{} ({}), whistles {}, walker {:.1f} m from its spot ({}; lead {:.1f} s; {} retargeted, {} steered, {} moved by the treadmill; record {:#x})", g_why,
			g_playerHold ? "paused by the player's command" : g_enabled && Cfg().buddyFollow ? "on" : "off", g_whistles, g_lastDist,
			g_hooked ? std::format("spot {:.1f} ahead / {:.1f} right", Cfg().buddyFollowAhead, Cfg().buddyFollowSide) : std::string("spot = Snake"),
			Cfg().buddyFollowLeadSec, g_retargets, g_steers, g_shifts, g_record) + std::format(", {} catch-ups; player commands {} (last {})", g_catchUps, g_playerCommands.load(), g_playerLast.load()) +
		       std::format("; method {}, walker mode {} (follows in mask {:#x}; {} updates left to its mode), {} record orders, {} whistles kept its mode, controller {:#x}",
				   MethodName(g_method), ModeName(g_lastMode), g_modeMask.load(), g_modeSkips, g_orders, g_modesKept.load(), g_walkerCtl.load());
	}
}
