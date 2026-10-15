#pragma once
// D-Horse treadmill ([Buddy] HorseTreadmill, default false; docs/GUEST.md 8.1a). TppHorse2 answers no SetPosition, so the
// treadmill used to leave the horse alone: no treadmill while Snake rides it (MGSV's airport limits the ride) and, on foot, a
// whistle after every jump (in Elden Ring it jumped by the radius, then galloped back). Here the horse is moved the way
// ActorProxy v1 moves soldiers: by its own location object (HorseLogic.h), found by a scan of MGSV's memory near its skin.
// Everything runs on the game thread (the camera frame, Frame.cpp), as Hold's placements. UNVERIFIED live.

#include "HorseLogic.h"
#include "State.h"

#include <cstdint>
#include <optional>
#include <string>

namespace phantom::guest::horse
{
	// Byte checks (the location class's constructor and setter); false = the feature stays off this session.
	bool Init();
	// Every camera frame (game thread), before the treadmill: finds and tracks the horse's location object, serves `!horse` tests.
	void Update(const state::LuaView& a_lua, std::uint64_t a_nowMs);
	// What the treadmill does with D-Horse on its next jump (a_ridden: Snake rides it).
	plan::Action Decide(const state::LuaView& a_lua, bool a_ridden);
	// Game thread: moves the horse by a_shift through its location object -- or, given a_groundY, horizontally by a_shift with its
	// feet put on that height (a ridden jump, plan::GroundedShift). Returns how far it moved (Snake's saddle moves as much); nothing
	// = not moved (the treadmill then does what it did without the feature); repeated failures make it unmovable for the session.
	std::optional<logic::Vec3> Shift(const logic::Vec3& a_shift, float a_fallbackYaw, const char* a_why, std::optional<float> a_groundY = std::nullopt);

	// A ridden jump left Snake to the saddle (no placement of ours): his root (a_root, before the jump) must land within
	// plan::RiddenWatch's window.
	void NoteRiddenJump(std::uint64_t a_frame, std::uint64_t a_nowMs, const logic::Vec3& a_root);
	void RiddenLanded();
	// Every frame (a_root: Snake's now): true once the window passed without his landing -- the horse was put back under him and
	// no ridden jump is tried again this session (Decide gives kNoTreadmill). The caller closes its own warp window.
	bool RiddenMissed(std::uint64_t a_frame, std::uint64_t a_nowMs, const logic::Vec3& a_root);

	// Console (`!horse`, `!horse scan`, `!horse shift <dx> <dz>`, `!horse on|off`).
	std::string Describe();
	void        RequestScan();
	// Empty = queued for the next frame; else why not (the switch off, or no confirmed object).
	std::string RequestTestShift(const logic::Vec3& a_shift);
}
