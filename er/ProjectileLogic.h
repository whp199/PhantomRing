#pragma once

// Pure rules of the projectile meshes (er/Projectiles.h): how the MGSV draws of a grenade / rocket seen once just after its
// release (protocol v13 kOwnerProjectile) become a template in the projectile's own frame, and how that template is put back
// at the host's flown copy, turned along its flight. No game access: tests/test_host_projectiles.inc runs all of it.
//
// Frames: the draw list's matrices are "anchor-relative MGSV world" (MGSV world minus the list's anchorPos, er/RenderScene.h),
// mapped to Elden Ring by the snapshot's anchor matrix A (world = A * p). A's 3x3 is a rotation about Y, possibly with an X
// mirror ([Axes]): orthonormal, so its inverse is its transpose -- a flown copy's Elden Ring point and direction go back
// into the list's space exactly, and the renderer's A brings them out again where the copy is.

#include "Math.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <span>
#include <vector>

namespace phantom::er::projectile
{
	// An orthonormal frame whose z is a_dir (forward), y as close to world up as it can be; a vertical a_dir takes world z as
	// its "up" instead. a_dir need not be unit; a zero/NaN one gives the identity.
	struct Basis
	{
		Vec3 x{ 1, 0, 0 }, y{ 0, 1, 0 }, z{ 0, 0, 1 };
	};
	Basis FlightBasis(const Vec3& a_dir);

	// One draw of the projectile: its record (mesh, textures, material...) and its matrices in the projectile's frame
	// (rotation in the flight basis, translation from the projectile's origin).
	struct Part
	{
		proto::DrawRecord  record{};
		std::vector<float> local;  // 12 floats per matrix
	};
	struct Template
	{
		std::vector<Part> parts;
		float             extent{ 0 };  // m: the farthest part origin from the projectile's origin (sanity)
		Vec3              at;           // the parts' mean origin in the list it was learned from (how it moved, Confirms)
		float             across{ -1 }; // m: its real size from the meshes' boxes (Across) once they are uploaded; -1 not yet
		bool              refused{ false };  // too big for what it was taken for (Across): drawn as the ball, replaceable
	};

	inline constexpr std::size_t kMaxParts = 6;
	inline constexpr float       kMaxExtent = 1.5f;  // m: a grenade or a rocket; anything bigger was not one (a mis-tag)
	// m: the real size a projectile may have (Across). A frag grenade is ~0.1 m, a rocket under 1 m; Snake's rifle is ~1 m and
	// must not pass as a grenade. (A sidearm is grenade-sized: only Confirms can tell that one apart.)
	inline constexpr float       kMaxThrownAcross = 0.5f;
	inline constexpr float       kMaxRoundAcross = 1.2f;

	// The mesh keys of a template, sorted: two lists show the same projectile when these agree.
	std::vector<std::uint64_t> Signature(const Template& a_t);

	// Did a candidate seen in guest frame a_frame0 (a_t0) and again in a_frame1 (a_t1) fly like the projectile released along
	// a_dir (MGSV world)? The same meshes, 3..20 MGSV frames apart (lists are blended between frames: a frame or two apart
	// can show almost no motion), moved at least kMinConfirmSpeed and within 60 degrees of the release direction. Snake's own
	// gear and a prop down range stay still relative to his root (the list's space) -- the review's mis-learns.
	inline constexpr float kMinConfirmSpeed = 2.5f;  // m/s
	bool Confirms(const Template& a_t0, std::uint64_t a_frame0, const Template& a_t1, std::uint64_t a_frame1, const Vec3& a_dir, float a_fps = 60.0f);

	// A part's mesh box (bind space, er/RenderAssets.h Mesh::bounds*); valid false: unknown (that part counts as a point).
	struct Box
	{
		bool  valid{ false };
		float min[3]{}, max[3]{};
	};
	// m: the diagonal of the box around all parts' mesh boxes, in the template's frame. a_boxes[i] belongs to a_t.parts[i].
	float Across(const Template& a_t, std::span<const Box> a_boxes);

	// A flown copy's look: its heading (ER world) and spin about it, held when it slows (a landed grenade keeps the pose it
	// came down in instead of snapping to world +z, level).
	struct Pose
	{
		Vec3  heading{ 0, 0, 1 };
		float spin{ 0 };   // rad
		float age{ 0 };    // s: the copy's age when last advanced
		bool  have{ false };
	};
	void Advance(Pose& a_pose, const Vec3& a_vel, float a_age, bool a_tumbles, float a_spinRate);

	// [Render] projectile_meshes off: the guest's kOwnerProjectile draws go back to where it put them before v13 -- Snake's
	// within 2.5 m of his root (the list's origin; a_keepNear false, [Render] draw_player off: out too), the rest out.
	inline constexpr float kHandBackRadius = 2.5f;
	void HandBack(std::vector<proto::DrawRecord>& a_draws, std::span<const float> a_matrices, bool a_keepNear);

	// The kOwnerProjectile draws of one list (others are skipped), flying along a_dir (MGSV world) then. Empty when there are
	// none, too many, or they spread wider than kMaxExtent.
	Template Learn(std::span<const proto::DrawRecord> a_draws, std::span<const float> a_matrices, const Vec3& a_dir);

	// The template at a_pos flying along a_dir (both in the list's space): records appended to a_draws (firstMatrix pointing
	// into a_matrices, which grows), owner kOwnerProjectile. a_spin (rad) turns it about its flight axis (a grenade tumbling).
	void Place(const Template& a_t, const Vec3& a_pos, const Vec3& a_dir, float a_spin, std::vector<proto::DrawRecord>& a_draws, std::vector<float>& a_matrices);

	// Elden Ring point / direction -> the list's space, through the inverse of the anchor matrix a_anchor (3x4 rows).
	Vec3 ToListPoint(const float a_anchor[12], const Vec3& a_world);
	Vec3 ToListDir(const float a_anchor[12], const Vec3& a_world);
	Vec3 FromListPoint(const float a_anchor[12], const Vec3& a_list);  // world = A * p (tests)

	// Which release a projectile's draws belong to: the newest one at most a_maxAgeMs old not yet settled. -1: none.
	struct Pending
	{
		std::uint64_t key{ 0 };
		double        atMs{ 0 };
		bool          learned{ false };
	};
	int PickRelease(std::span<const Pending> a_pending, double a_nowMs, double a_maxAgeMs);
}
