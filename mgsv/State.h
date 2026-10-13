#pragma once
// What the Lua pump (Pump.cpp) and the player-camera frame (Frame.cpp) tell each other. They may run
// on different game threads; everything here is copied in and out under one small lock.

#include "Logic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace phantom::guest::state
{
	// PhantomRing.Tick flag bits (GuestScript.cpp PR.F).
	enum LuaFlags : std::uint32_t
	{
		kLTitle = 1u << 0,
		kLPrepared = 1u << 1,
		kLDemo = 1u << 2,
		kLMenu = 1u << 3,
		kLDead = 1u << 4,
		kLVehicle = 1u << 5,
		kLDash = 1u << 6,
		kLStand = 1u << 7,
		kLSquat = 1u << 8,
		kLCrawl = 1u << 9,
		kLCqc = 1u << 10,
		kLSubject = 1u << 11,
		kLFall = 1u << 12,
		kLBinocle = 1u << 13,
		kLNormal = 1u << 14,
		kLHeli = 1u << 15,
		kLGunFire = 1u << 16,
		kLCarry = 1u << 17,
		kLSeqGame = 1u << 18,
		kLBehind = 1u << 19,
		kLCbox = 1u << 20,    // PlayerStatus CBOX: inside a cardboard box
		kLPoster = 1u << 21,  // ...one with a poster on it (vars.cboxPosterIndices of the current item)
		kLNvg = 1u << 22,     // night-vision goggles on (vars.items[vars.currentItemIndex] == EQP_IT_Nvg)
	};

	// BuddyType.WALKER_GEAR (VERIFIED from the enum registration at 0x140A46E21, mgsvtpp.exe 1.0.15.4;
	// 3 is QUIET). LuaView::buddyActive holds whichever buddy is out, not necessarily ours.
	inline constexpr int kBuddyTypeWalker = 4;
	inline constexpr int kBuddyTypeHorse = 1;  // BuddyType.HORSE (live 2026-10-13: GetActiveBuddyType 1 with D-Horse out)

	struct LuaView
	{
		bool          valid{ false };
		std::uint64_t tickMs{ 0 };  // GetTickCount64 of the last successful Tick
		std::uint32_t flags{ 0 };
		int           mission{ 0 };
		std::string   sequence;
		int           buddyActive{ -1 };  // BuddyType value of the active buddy block, -1 none
		std::uint16_t buddyId{ 0xFFFF };
		bool          buddyPosValid{ false };
		logic::Vec3   buddyPos;
		bool          buddyYawValid{ false };
		float         buddyYaw{ 0 };
		std::uint16_t ridden{ 0xFFFF };   // GameObject Snake rides (Player.GetGameObjectIdIsRiddenToLocal)

		[[nodiscard]] bool InGame() const
		{
			return valid && !(flags & kLTitle) && (flags & kLPrepared) && (flags & kLSeqGame) && !(flags & kLDemo);
		}
	};

	struct FrameView
	{
		bool          valid{ false };
		std::uint64_t frame{ 0 };
		std::uint64_t tickMs{ 0 };
		logic::Vec3   root;
		float         yaw{ 0 };
		logic::Vec3   camPos;
		logic::Vec3   velocity;  // Snake's smoothed root velocity (m/s), what the host also gets
	};

	enum class Boot : int
	{
		kWaitLua,
		kTitle,
		kWaitGame,
		kLoadSandbox,
		kSettle,
		kSetup,
		kBuddy,
		kBox,  // the poster on Snake's box (MGSV's item menu, docs/GUEST.md 18.9)
		kCentre,
		kReady,
		kFailed,
	};
	const char* BootName(Boot a_boot);

	void    SetLua(const LuaView& a_view);
	LuaView Lua();
	void      SetFrame(const FrameView& a_view);
	FrameView Frame();

	// Work that must run on MGSV's Lua thread a few times a second (follow::Update), registered by Guest.cpp so the
	// pump (and its test) doesn't depend on it.
	using LuaThreadWork = void (*)(std::uint64_t a_nowMs, bool a_ready);
	void SetLuaThreadWork(LuaThreadWork a_work);
	void RunLuaThreadWork(std::uint64_t a_nowMs, bool a_ready);
	// The same, after every PhantomRing.Tick (~30 Hz, also while MGSV's pause menu stops the player-camera frames): the
	// menu probe and the GuestState republished while frames stall (Guest.cpp).
	void SetLuaTickWork(LuaThreadWork a_work);
	void RunLuaTickWork(std::uint64_t a_nowMs, bool a_ready);

	// The buddy's own skin (Skeleton.cpp) and D-Horse's whistle (Follow.cpp), registered by Guest.cpp so Pump.cpp stays
	// testable on its own: no-ops / false until registered.
	struct BuddyHooks
	{
		bool (*pose)(logic::Vec3& a_pos, float& a_yaw){ nullptr };
		void (*hint)(const logic::Vec3& a_at, float a_radius, std::uint64_t a_forMs){ nullptr };
		bool (*whistleHorse)(const char* a_why){ nullptr };
		void (*forget)(const char* a_why){ nullptr };
	};
	void SetBuddyHooks(const BuddyHooks& a_hooks);
	// `!buddy <type>` / the iDroid: deploy another buddy in session (Pump.cpp re-runs its buddy step on MGSV's Lua thread).
	void RequestBuddy(const std::string& a_type);
	std::optional<std::string> TakeBuddyRequest();
	bool BuddyPose(logic::Vec3& a_pos, float& a_yaw);
	void HintBuddy(const logic::Vec3& a_at, float a_radius, std::uint64_t a_forMs);
	bool WhistleHorse(const char* a_why);
	void ForgetBuddy(const char* a_why);

	// One of MGSV's own menus owns the pad (the pause menu, the iDroid, a popup): Menu.cpp sets it, the boot
	// sequence keeps the sandbox "ready" through it (the pause menu stops the player-camera frames).
	void SetMenuOpen(bool a_open);
	bool MenuOpen();

	// Frame -> pump: Lua commands for the next Tick ("warp x y z yaw;owarp id x y z yaw").
	void        QueueCommand(const std::string& a_command);
	std::string TakeCommands();
	// Pump -> whoever asked: what PhantomRing.Tick's PR.Run answered for those commands ("warp=ok refill=36/30/0,..."), on
	// MGSV's Lua thread right after the Tick (Ammo.cpp applies the refill there). Registered by Guest.cpp.
	using CommandResultSink = void (*)(std::string_view a_results);
	void SetCommandResultSink(CommandResultSink a_sink);
	void RunCommandResults(std::string_view a_results);

	void SetBoot(Boot a_boot);
	Boot CurrentBoot();
	void RequestBootRestart();
	bool TakeBootRestart();

	// Console -> game threads: retarget the sandbox (consumed by the boot step) or change how Snake is
	// held (consumed by the frame), without a restart.
	struct SandboxRequest
	{
		int   mission{ 0 };
		bool  hasPosition{ false };
		float position[3]{};
		float rotYDeg{ 0 };
	};
	void                          RequestSandbox(const SandboxRequest& a_request);
	std::optional<SandboxRequest> TakeSandboxRequest();

	struct HoldRequest
	{
		int   mode{ 0 };  // HoldMode
		float radius{ 0 };
	};
	void                       RequestHold(const HoldRequest& a_request);
	std::optional<HoldRequest> TakeHoldRequest();

	void                       SetCentre(std::optional<logic::Vec3> a_centre);
	std::optional<logic::Vec3> Centre();

	void SetTreadmillEnabled(bool a_on);
	bool TreadmillEnabled();

	// "Tarnished mode" (protocol v10 kHEvTarnished): the player plays the Tarnished, Snake parks where he stands -- the pad
	// is neutral (Pad.cpp), no treadmill warps (Frame.cpp), no follow whistles or steering for the buddy (Follow.cpp).
	void SetParked(bool a_parked);
	bool Parked();

	// The configured buddy isn't available in this save (not developed, dead, can't sortie).
	void SetBuddyMissing(bool a_missing);
	bool BuddyMissing();
}
