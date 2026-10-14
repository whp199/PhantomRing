#pragma once

// MGSV's Fulton recovery on Elden Ring's enemies (protocol v12 kGEvFulton; 2026-10-14, the player: "would love FULTON
// extraction of sleeping Elden Ring enemies"). The guest's ActorProxy stands an MGSV soldier in for an Elden Ring character;
// when Snake fultons that soldier, MGSV's own balloon goes up and the guest names the Elden Ring character it stood for.
//
// Here that character leaves Elden Ring's world MGSV's way, with no death, no drop and no kill: a balloon inflates over him
// (the world markers, er/RenderMarkers.h), he is held (no move, no attack, no gravity) and lifted, slowly and then fast, for
// kSeconds, then disabled: ChrIns debug flag character_disabled (+0x538 bit 12, fromsoftware-rs: no updates, no rendering, no
// physics). Nothing targets, marks or shoots a disabled character (game::Characters leaves them out). Until Elden Ring
// brings its enemies back (a rest, a death, a load) a fultoned one that turns up again (its block reloaded) is disabled again;
// after that it is back like any enemy. Every write goes through SafeWrite; a character that is no longer the one fultoned
// (its handle) is left alone.

#include "Math.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace phantom::proto
{
	struct GuestEvent;
}

namespace phantom::er
{
	struct Frame;

	namespace Fulton
	{
		inline constexpr float kSeconds = 2.2f;  // inflate, lift, yank: then gone

		// Pure (tests). The look at t seconds after the balloon went up: the balloon's size (0..1), how far the character has
		// risen (m), and whether he is still in the world.
		struct Lift
		{
			float balloon{ 0 };
			float rise{ 0 };
			bool  done{ false };
		};
		Lift LiftAt(float a_t);

		// Pure (tests): which character the event means -- the one with its handle, else (handle unknown or not near: an
		// Elden Ring origin shift, a proxy that drifted) the nearest living, not disabled one within a_radius of where the proxy
		// stood; -1 = none.
		struct Candidate
		{
			std::uint64_t handle{ 0 };
			Vec3          pos;
			bool          alive{ true };
		};
		int Pick(std::span<const Candidate> a_chrs, std::uint64_t a_handle, const Vec3& a_at, float a_radius);

		void OnEvent(const Frame& a_frame, const proto::GuestEvent& a_event);  // kGEvFulton
		void Tick(const Frame& a_frame);                                       // FrameBegin, after the events
		void OnEnemiesBack(const char* a_why);  // a rest, a respawn, a load: Elden Ring brings its enemies back -- forget ours
		void OnWorldShift(const Vec3& a_delta);
		std::string Describe();
		// `fulton [status | nearest [fail] | 0x<handle> [fail] | forget]`: a kGEvFulton as the guest would send it (tests without
		// MGSV); forget: as a rest (the fultoned ones are no longer kept out).
		std::string Command(const Frame& a_frame, std::string_view a_args);
	}
}
