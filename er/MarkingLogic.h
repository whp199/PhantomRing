#pragma once

// The pure part of binocular marking (er/Marking.h; tests/test_host_marking.inc): which Elden Ring characters Snake marks by
// looking at them through MGSV's binoculars, and how a marked one's MGSV-style marker is laid out in the world.
//
// MGSV marks a soldier once he has stayed near the centre of the binoculars' view for a moment with nothing in between; the
// mark stays for the rest of the mission (a red triangle over him, his distance under it, through walls). Here the same:
//  - "near the centre" is an angle from the view axis that is a share of the camera's half vertical FOV, so it narrows as
//    the binoculars zoom in (the same part of the screen), never under min_cone_deg; a character's body counts, not only
//    his chest point (a close one that fills the view is in it);
//  - looking for dwell seconds marks him (the time adds up while he is in the cone, ebbs away twice as fast when he leaves);
//  - a line-of-sight check (Elden Ring's own ray cast in the game, a function here) is made only for the few in the cone; a
//    ray that stops on his own hit capsule still sees him (a giant's body is far wider than any fixed slack);
//  - a marked one who dies keeps his marker for fade seconds, fading, then is forgotten; one who is not in this frame's list
//    (out of range, his block unloaded, fultoned, hidden by an event) keeps his mark and shows again when he is back;
//  - marks last until the next world load (LoadWatch: fast travel, a death, the title, another save), as MGSV's last a mission.

#include "CombatLogic.h"
#include "Math.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::er
{
	struct Marker;
}

namespace phantom::er::marking
{
	struct Rules
	{
		float       range{ 150.0f };       // m from the camera
		float       coneShare{ 0.12f };    // of the camera's half vertical FOV
		float       minConeDeg{ 1.0f };    // the cone is never narrower
		float       bodyRadius{ 0.6f };    // m: a character's body around its aim point
		float       dwell{ 0.5f };         // s of looking that marks
		float       fade{ 1.0f };          // s a dead one's marker fades
		std::size_t maxMarked{ 64 };       // the oldest mark goes when a new one would be one too many
		std::size_t maxRays{ 6 };          // line-of-sight checks per update, the ones nearest the centre first
	};

	struct View
	{
		Vec3  pos;
		Vec3  fwd;              // normalised or not
		float fovY{ 0.8f };     // rad, vertical
		bool  binoculars{ false };
	};

	struct Seen
	{
		std::uint64_t handle{ 0 };
		Vec3          aim;              // the body's centre (chest), Elden Ring world
		bool          alive{ true };
		// His own hit capsule (Elden Ring's physics module), when known: a big one's body widens what the cone sees of him (never
		// under Rules::bodyRadius), and a ray stopping on it still sees him (RayClear).
		std::optional<combat::Capsule> body;
	};

	// true = nothing in between a_from and a_target.aim
	using LineOfSight = std::function<bool(const Vec3& a_from, const Seen& a_target)>;

	// A ray cast from a_from to a_to stopped at a_hit: is a_to still seen? Yes when it stopped within kLosSlack of a_to (a cape,
	// an arm in front of his chest), or on a_body itself, give or take kLosBodySlack: when Elden Ring's ray stops on the body we
	// look at, a troll's capsule (1.5 m and more wide) stops it metres short of a point 1.2 m over his feet.
	inline constexpr float kLosSlack = 1.0f;      // m
	inline constexpr float kLosBodySlack = 0.3f;  // m
	bool RayClear(const Vec3& a_from, const Vec3& a_to, const Vec3& a_hit, const std::optional<combat::Capsule>& a_body);

	// Marks belong to one Elden Ring world: a new one starts after a load screen of at least kMinLoadMs (fast travel, a death,
	// the title, another save: placed characters come back with the same handles, runtime ones may reuse them), or when the
	// Tarnished's ChrIns changes under us. Update says when to forget them (once per new world).
	class LoadWatch
	{
	public:
		static constexpr std::uint64_t kMinLoadMs = 500;  // shorter drop-outs (a few frames with no player) are not a load
		// a_inWorld: Frame::inGame; a_player: the Tarnished's ChrIns (0: none); a_nowMs: a steady clock.
		bool Update(bool a_inWorld, std::uintptr_t a_player, std::uint64_t a_nowMs);

	private:
		bool           loading_{ false };
		std::uint64_t  since_{ 0 };   // when this load began
		std::uintptr_t player_{ 0 };  // the last ChrIns seen in the world
	};

	// The cone's half angle (rad) for a view.
	float ConeHalfAngle(const View& a_view, const Rules& a_rules);
	// The angle (rad) between the view axis and the nearest part of a body of a_rules.bodyRadius around a_aim (0 = the axis goes
	// through it); a_dist gets the distance. Negative distance along the axis (behind) gives pi.
	float OffAxis(const View& a_view, const Vec3& a_aim, const Rules& a_rules, float* a_dist = nullptr);

	class Marks
	{
	public:
		// One update (task thread, once per frame): dwell and marks from what the view sees, deaths faded. Returns the handles
		// newly marked by it.
		std::vector<std::uint64_t> Update(const View& a_view, std::span<const Seen> a_seen, float a_dt, const Rules& a_rules, const LineOfSight& a_los);

		bool  Marked(std::uint64_t a_handle) const;
		float Opacity(std::uint64_t a_handle, float a_fade) const;  // 1 marked and alive, to 0 over a_fade s after a death; 0 not marked
		float Age(std::uint64_t a_handle) const;      // s since marked (the marker's pop), -1 not marked
		float Dwell(std::uint64_t a_handle) const;    // s looked at so far (not marked yet), 0 none
		bool  Mark(std::uint64_t a_handle, const Rules& a_rules);  // marks at once (console); false: already marked
		void  Forget(std::uint64_t a_handle);
		void  Clear();
		std::size_t Count() const;         // marked
		std::size_t Looking() const;       // with dwell, not marked yet
		std::uint64_t Rays() const { return rays_; }

	private:
		struct Entry
		{
			std::uint64_t handle{ 0 };
			float         dwell{ 0 };
			bool          marked{ false };
			float         age{ 0 };
			float         deadFor{ -1 };  // s since a marked one was seen dead, -1 alive
			std::uint64_t order{ 0 };     // when marked (the oldest goes first)
		};
		Entry*       Find(std::uint64_t a_handle);
		const Entry* Find(std::uint64_t a_handle) const;
		void         Trim(const Rules& a_rules);

		std::vector<Entry> e_;
		std::uint64_t      next_{ 0 };
		std::uint64_t      rays_{ 0 };
	};

	// ---- the marker's look -------------------------------------------------------------------------------------------------
	struct Style
	{
		float size{ 0.012f };       // the triangle's half height, share of the screen height
		float textSize{ 0.6f };     // a digit's half height, share of the triangle's
		float hostile[4]{ 1.0f, 0.07f, 0.04f, 1.0f };  // linear RGBA: MGSV's red for enemies
		float other[4]{ 0.85f, 0.85f, 0.85f, 1.0f };   // everyone else (white)
		float text[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
	};

	struct Camera
	{
		Vec3  pos, right, up, fwd;  // right/up/fwd normalised
		float fovY{ 0.8f };
	};

	// "37m": whole metres, at most 9999 (glyphs 0-9 and 10 = 'm' for the marker shader).
	std::string DistanceText(float a_metres);
	std::vector<std::uint8_t> Glyphs(std::string_view a_text);

	// The quads of one marker: a triangle pointing down at a_at (over the character's head) and, when a_text isn't empty, its
	// glyphs centred over it, all the same size on screen at any distance (they scale with the distance and the FOV). A newly
	// marked one's triangle pops (a_age < 0.3 s: up to 1.6x); the glyphs stay put. Shown through walls. Nothing when a_at is
	// behind the camera.
	void Build(std::vector<Marker>& a_out, const Camera& a_cam, const Vec3& a_at, std::string_view a_text, bool a_hostile, float a_opacity, float a_age,
		const Style& a_style);
}
