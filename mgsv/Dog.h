#pragma once
// D-Dog as a battle buddy in Elden Ring (docs/research/ddog.md; protocol v17 kGEvDogAttack / kBuddyEngaged / kActorDogTarget;
// [Dog] in PhantomRingGuest.ini; the pure rules in DogLogic.h).
//
//  - Observes MGSV's dog: its message handler (0x140C56B10: the dog system, its style byte +0xD4, the orders it takes), its attack
//    state (0x140C64DE0: the run, the leap, the bite's contact) and TppSoldier2's dog-attack handler (0x141491A70: the soldier that
//    accepted the bite). Observe only: nothing is changed in those calls.
//  - [Dog] Treadmill: the dog moved with Snake's treadmill jumps through its own location object (BuddyLocation.h), so Elden Ring
//    doesn't see it jump ~6 m and its Wait keeps its place; a free dog left far behind out of view is put beside Snake (the leash).
//  - 0.8.0 polish (docs/research/ddog.md §9): taken along to MGSV's ladder / ledge sites with Snake and back (StageAlong); each
//    placement watched for MGSV undoing it; what Elden Ring is shown (published place and draws) never faster than the dog runs
//    (MaxShownSpeed) and, while it waits, where Elden Ring saw it wait (WaitKeepsPlace: the slip in ActorProxy's SlipObserver);
//    its soldier held while it bites (ActorProxy HoldDedicated); MGSV's markers out of the HUD layer during an order.
//  - [Dog] Attack: the call menu's attack commands (11-15) go to the Elden Ring enemy the host picks (kActorDogTarget): one inert
//    MGSV soldier stands at him (ActorProxy Dedicate), MGSV's own attack order sends the dog at that soldier, and the soldier
//    accepting the bite is kGEvDogAttack Connected (with MGSV's damage share), then Released; Refused / Lost otherwise.
//    kBuddyEngaged in between. The fetch ("Get it") takes the target's asleep proxy and MGSV's Fulton becomes kGEvFulton as for Snake.
// Threads: the detours run where MGSV calls them (game thread); Update and Shift on the game thread (the camera frame, Frame.cpp);
// LuaTick on MGSV's Lua thread (Guest.cpp, after every Tick); the console anywhere.

#include "Logic.h"
#include "State.h"

#include <cstdint>
#include <optional>
#include <string>

namespace phantom::guest::dog
{
	// DllMain: byte checks and the three observe-only detours (each behind its checks; a mismatch disables the dog layer, logged).
	void Install();

	struct FrameIn
	{
		bool          ready{ false };     // MGSV ready, Snake on foot, not staged / parked / anchored
		float         dt{ 0.0f };
		std::uint64_t nowMs{ 0 };
		logic::Vec3   root;               // Snake's root (MGSV world)
		logic::Vec3   velocity;           // the odometer's smoothed velocity
		logic::Vec3   camPos;             // MGSV's camera
		bool          staged{ false };    // Snake is staged at an MGSV ladder or ledge (Climb.h): the dog goes along with him
		bool          onFoot{ true };
		logic::Vec3   home;               // ...where he stood on the sandbox when the move began
		float         yaw{ 0.0f };        // Snake's facing
	};
	// Game thread, every camera frame before the treadmill: the dog's location object, last frame's treadmill shift, the leash,
	// the transform query (`!dog where`).
	void Update(const state::LuaView& a_lua, const FrameIn& a_in);
	// The treadmill / the Fulton anchor moved Snake by a_shift this frame: the dog too ([Dog] Treadmill), at the next frame (when Snake's
	// own placement shows in his root and draws). False = not moved (left to MGSV as before).
	bool Shift(const logic::Vec3& a_shift, const char* a_why);
	// The dog's feet from its location object (this frame's) while it is confirmed; nothing otherwise (use the skin's).
	std::optional<logic::Vec3> Position();
	// Frame.cpp, every frame right before the GuestState goes out: one line of `!dog placetrace` (PhantomRingDogTrace.csv next to the
	// DLL) -- Snake's root and odometer, the published buddyPos (a_fromLocation: from its location object, else the skin's), and the
	// dog by every source: its location object, its skin, the Lua view, its position cache, our placements, the attack state, the job
	// and its soldier. Nothing while the trace is off.
	void TraceFrame(std::uint64_t a_frame, const logic::Vec3& a_root, const double a_odo[3], const float a_pub[3], const logic::Vec3& a_raw, bool a_fromLocation,
		std::uint32_t a_guestFlags, std::uint32_t a_buddyFlags);
	// Frame.cpp at the publish, every frame D-Dog is out: the place to publish (MGSV world) for a_place -- its location object's feet
	// (or its skin's), relative to Snake's home while staged -- limited to the dog's own speed in Elden Ring's terms (DogLogic.h
	// DisplayLimiter: MGSV's lunge and its every-other-frame updates no longer step over ~0.25 m a frame). The dog's draws are moved
	// by the difference (capture::NoteBuddyOffset). a_epoch: the odometer's epoch (a new one starts over).
	logic::Vec3 Display(const logic::Vec3& a_place, const logic::Vec3& a_root, const double a_odo[3], std::uint32_t a_epoch, float a_dt);
	// The move that took the dog along to Snake's staging site (zero when none): published relative to his home without it.
	logic::Vec3 StagedMove();
	// kBuddyEngaged: an attack job is connected (any thread).
	bool Engaged();

	// MGSV's Lua thread after every Tick: the attack job, the console's orders, the style and order record reads.
	void LuaTick(std::uint64_t a_nowMs, bool a_ready);

	// Follow.cpp's command detour, for the player's own buddy commands (the call menu): a_command 8-16 while D-Dog is out. True = the
	// command was taken over (an attack order for the Elden Ring target: MGSV's original isn't called now). Also notes Wait / whistle.
	bool OnPlayerCommand(int a_command, std::uint16_t a_arg);
	// kGEvBuddyOrder pad[0] for a dog command: DogStyle + 1 of the dog's equipment for 11-15, 0 = unknown / not an attack.
	std::uint32_t OrderStylePad(int a_command);
	// GuestState buddyFlags' kBuddyDogStyleMask bits (v17): the dog's DogStyle + 1 as its style byte reads now (0 = unknown).
	std::uint32_t BuddyStyleBits();

	// Capture.cpp: a buddy-owned draw within a proxy's capture radius is kept when its mesh is one of the dog's (learned from draws rooted
	// on the dog's skin while no proxy was within 4 m). a_srcHash = the mesh source's pointer-level hash; a_nearProxy = a proxy (or a
	// just-released one) is within 4 m; a_origin = the draw's m_world translation (MGSV world).
	bool KeepBuddyDraw(std::uint64_t a_srcHash, bool a_nearProxy, const float a_origin[3]);

	// ActorProxyGame.cpp's phase lock (Lua thread): MGSV's alert phase couldn't be held at 0 while the dog's soldier was in the world --
	// [Dog] Attack is off for the rest of the session (logged; `!dog attack on` tries again).
	void DisableAttack(const std::string& a_why);

	// Console: `!dog` (state), `!dog on|off`, `!dog treadmill on|off`, `!dog attack on|off`, `!dog where`, `!dog scan`,
	// `!dog shift <dx> <dz>`, `!dog order <cmd> [arg]`, `!dog style <0-7>` (test: the style byte in memory), `!dog leash <m>`,
	// `!dog placetrace on|off` (TraceFrame's CSV).
	std::string Command(const std::string& a_args);
	std::string Describe();
}
