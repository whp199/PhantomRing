#pragma once
// Quiet as a sniper buddy in Elden Ring, and her rest at a Site of Grace (docs/research/quiet.md; protocol v18 kHostQuietLayer /
// kHostAtGrace, HostState's post and grace fields, buddyFlags kBuddyCloaked / kBuddySeated / kBuddyAtPost / her weapon /
// kBuddyQuietCover / kBuddyCanRest / kBuddyQuietMarked, kGEvQuietShot; [Quiet] in PhantomRingGuest.ini; the pure rules in
// QuietLogic.h).
//
//  - Observes MGSV's Quiet: her message handler (0x140CE3330: the Quiet system, her order record R through two loads), her hit's
//    sender (0x140D04D50: every rifle hit with its soldier and attack id) and her "can't" voice (0x140D01410). Observe only.
//  - The borrow (§3.1): on a save without a deployable Quiet, the TppBuddyService bits of her state's plan are set in memory only,
//    while the save guards are complete, re-applied after sandbox loads, and put back with a raw byte diff (`!quiet borrow off`, a
//    guard lost, Elden Ring gone). MGSV's save is never written.
//  - Her place (§3.2): her own location object (BuddyLocation.h), held at the host's post mapped into MGSV (the mirror, pulled into
//    the apron's free region), moved with Snake's treadmill jumps.
//  - Orders (§3.5): the player's Fire / Take aim go to the Elden Ring enemy the host picks (kActorQuietTarget): a dedicated inert
//    stand-in soldier (ActorProxy), MGSV's own command on him, her hit on him = kGEvQuietShot Hit. Cover me and the talk are taken
//    over while the host's layer is on.
//  - The grace (§3.9): seated at the host's grace seat (placed, then her rifle stood down), up again when kHostAtGrace falls.
// Threads: the detours run where MGSV calls them (game thread); Update / Shift / TraceFrame on the game thread (Frame.cpp);
// LuaStep / BeforeDeploy / AtReady on MGSV's Lua thread (Pump.cpp, with a Lua call); the console anywhere.

#include "Logic.h"
#include "State.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace phantom::guest::quiet
{
	using LuaCall = std::function<std::string(const std::string&)>;

	// DllMain: byte checks and the observe-only detours (a mismatch turns the Quiet layer off, logged).
	void Install();

	struct FrameIn
	{
		bool          ready{ false };   // MGSV ready, Snake on foot, not staged / parked / anchored
		float         dt{ 0.0f };
		std::uint64_t nowMs{ 0 };
		logic::Vec3   root;             // Snake's root (MGSV world)
		logic::Vec3   velocity;
		logic::Vec3   camPos;
		double        odo[3]{};         // the odometer (for the treadmill offset k = root - odo)
		float         yaw{ 0.0f };
	};
	// Game thread, every camera frame before the treadmill: her location object, the holder (post / seat / console), last frame's
	// treadmill shift, the host's post and seat read and mapped.
	void Update(const state::LuaView& a_lua, const FrameIn& a_in);
	// The treadmill / the Fulton anchor moved Snake by a_shift: her too, at the next frame (false = not moved).
	bool Shift(const logic::Vec3& a_shift, const char* a_why);
	// Her feet from her location object (this frame's) while it is confirmed.
	std::optional<logic::Vec3> Position();

	// MGSV's Lua thread, Pump's ready loop: the order job, the sit's Enable*, FireOnlyOnOrders / NoDying, the borrow's re-checks and
	// the console's Lua work.
	// a_guarded: MGSV's saves are blocked and every guard is in place (Pump's SavesGuarded): a borrow is put back the moment it isn't.
	void LuaStep(std::uint64_t a_nowMs, bool a_ready, bool a_guarded, const LuaCall& a_call);
	// MGSV's Lua thread, the boot's buddy step (before PR.Buddy), for [Buddy] Type = quiet / `!buddy quiet` and after every sandbox load:
	// the borrow (re-)applied when the save's bits want it. Returns a status line to log, or "" when nothing was done.
	std::string BeforeDeploy(bool a_guarded, const LuaCall& a_call);
	// MGSV's Lua thread, the sandbox ready (boot's end): [Quiet] BorrowListInIdroid borrows now, so MGSV's BUDDY SUPPORT lists her.
	void AtReady(bool a_guarded, const LuaCall& a_call);

	// Pump: hold the periodic ClearMarkers off ([Quiet] KeepMarks: while a stand-in is dedicated; `!quiet keepmarks on`).
	bool KeepMarkers();
	// Follow.cpp's command detour, the player's own 1-7 while Quiet is out. True = taken (MGSV's original isn't called).
	bool OnPlayerCommand(int a_command, std::uint16_t a_arg);
	// kGEvBuddyOrder pad[0] for a Quiet command: QuietWeapon + 1 for 1-3, 0 otherwise / unknown.
	std::uint32_t OrderWeaponPad(int a_command);
	// GuestState: Quiet's buddyFlags bits (kBuddyCloaked, kBuddySeated, kBuddyAtPost, her weapon, kBuddyQuietCover, kBuddyCanRest,
	// kBuddyQuietMarked, kBuddyEngaged) and the echoes. Frame.cpp, every frame Quiet is out.
	std::uint32_t BuddyBits();
	std::uint32_t PostSeqEcho();
	std::uint32_t GraceSeqEcho();
	// Her published heading: the skin root's (fresh) else the mirror's; nullopt = keep Frame's.
	std::optional<float> Yaw();
	// Capture.cpp: whether a buddy-owned draw near her is hers (skinned on her root; rigid within RigidRadius once learned). nullopt =
	// the rule doesn't apply (Quiet not out / the layer off): the old 4 m rule.
	std::optional<bool> KeepBuddyDraw(bool a_skinned, std::uint64_t a_meshKey, const float a_origin[3]);
	// Hud: MGSV's marker hues out while a stand-in is dedicated or an order runs ([Quiet] HideMgsvMarkers).
	bool HideMarkers();

	// ActorProxyGame.cpp's phase lock gave up with Quiet's stand-in out: her orders are off for the session (`!quiet orders on`).
	void DisableOrders(const std::string& a_why);

	// Frame.cpp, every frame Quiet is out: one line of `!quiet trace on` (PhantomRingQuietTrace.csv next to the DLL).
	void TraceFrame(std::uint64_t a_frame, const logic::Vec3& a_root, const float a_pub[3], std::uint32_t a_buddyFlags);

	// Console: `!quiet` (state), `!quiet r`, `!quiet scan`, `!quiet shift <dx> <dz>`, `!quiet hold <x> <z> [yaw]|rel <dx> <dz>|off`,
	// `!quiet where`, `!quiet borrow [on|off|test-unguarded]`, `!quiet bits`, `!quiet diff`, `!quiet keepmarks on|off`, `!quiet standin
	// <0xhandle>|off`, `!quiet trace on|off`, `!quiet hits`, `!quiet orders on|off`, `!quiet layer on|off`.
	std::string Command(const std::string& a_args);
	std::string Describe();
}
