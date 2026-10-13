#pragma once
// The guest's pure arithmetic: odometer, treadmill, life bridge, shot detection, camera maths.
// No game, Windows or mapping access here -- tests/test_guest_logic.cpp exercises all of it offline.

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace phantom::guest::logic
{
	struct Vec3
	{
		float x{}, y{}, z{};

		Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
		[[nodiscard]] float Length() const { return std::sqrt(x * x + y * y + z * z); }
		[[nodiscard]] float LengthXZ() const { return std::sqrt(x * x + z * z); }
		[[nodiscard]] bool  Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
	};

	// ---- Fox transforms -------------------------------------------------------------------------
	// Fox matrices are row-major with the translation in [12..14]; the model faces +Z (MGS5VR
	// head_camera.cpp), so yaw = atan2(forward.x, forward.z) with forward = row 2.
	Vec3  MatrixTranslation(const float a_m[16]);
	float MatrixYaw(const float a_m[16]);
	// Camera-local forward is +Z in Fox (MGS5VR cabin_walk.hpp).
	Vec3  RotateByQuat(const float a_q[4], const Vec3& a_v);
	// Yaw (atan2(forward.x, forward.z)) of a rotation quaternion xyzw; 0 for a degenerate one.
	float QuatYaw(const float a_q[4]);
	// The rotation quaternion xyzw about +Y with that yaw (QuatYaw(YawQuat(y)) == y): a facing to store where MGSV keeps a
	// rotation, e.g. D-Walker's destination record.
	std::array<float, 4> YawQuat(float a_yaw);
	// A point a_ahead metres in front of someone at a_at facing a_yaw and a_side metres to his right (negative: left), at
	// his height. MGSV's world is right-handed: facing +Z his right is -X (VERIFIED live: the buddy call spot, GUEST.md 18.3).
	Vec3  BesideOf(const Vec3& a_at, float a_yaw, float a_ahead, float a_side);
	// Where someone at a_at moving at a_velocity will be in a_seconds (horizontal only, at most a_maxAhead metres on);
	// a_at itself below a_minSpeed (standing, shuffling: no lead).
	Vec3  LeadPoint(const Vec3& a_at, const Vec3& a_velocity, float a_seconds, float a_maxAhead, float a_minSpeed);
	Vec3  TransformPoint(const float a_m[16], const Vec3& a_p);
	// Rotation part of a row-major (row-vector) matrix as a unit quaternion xyzw, i.e. the q with
	// RotateByQuat(q, v) == v * M for the basis rows. Rows are normalised first (Fox roots may carry scale).
	std::array<float, 4> MatrixToQuat(const float a_m[16]);
	// Focal length (mm, 35 mm-film convention) -> vertical FOV (rad).
	float FocalToFovY(float a_focalMm, float a_filmHeightMm);

	// ---- odometer -------------------------------------------------------------------------------
	// Snake's intended motion: the running sum of his frame-to-frame root displacement, with every
	// teleport (our treadmill warps, MGSV's own warps, respawns) left out. A frame counts as a
	// teleport when its displacement is bigger than a_teleportJump, or bigger than 0.75 m while we
	// know one of our warps is pending (a warp a few metres long would otherwise pass as motion).
	class Odometer
	{
	public:
		void Reset(const Vec3& a_root);
		// Returns true if this frame was a teleport (not added to the odometer).
		bool Update(const Vec3& a_root, double a_dtSeconds, bool a_warpPending, float a_teleportJump, float a_smoothing);
		// We moved Snake ourselves to a_root (a native hold/warp we know landed): next frame's motion is
		// measured from there, and the jump never counts.
		void Rebase(const Vec3& a_root)
		{
			if (a_root.Finite()) {
				last_ = a_root;
			}
		}

		[[nodiscard]] const std::array<double, 3>& Odo() const { return odo_; }
		[[nodiscard]] Vec3                          Velocity() const { return velocity_; }
		[[nodiscard]] bool                          Primed() const { return primed_; }
		[[nodiscard]] std::uint32_t                 Epoch() const { return epoch_; }
		[[nodiscard]] std::uint64_t                 Teleports() const { return teleports_; }

	private:
		std::array<double, 3> odo_{};
		Vec3                  last_{};
		Vec3                  velocity_{};
		bool                  primed_{ false };
		std::uint32_t         epoch_{ 0 };
		std::uint64_t         teleports_{ 0 };
	};

	// ---- treadmill ------------------------------------------------------------------------------
	struct TreadmillInput
	{
		Vec3   root;
		Vec3   centre;
		Vec3   velocity;
		float  radius{ 15.0f };
		bool   allowed{ true };    // not mid-CQC/dive/fall/menu/cutscene (and not on a vehicle unless enabled)
		double nowMs{ 0 };
		double lastWarpMs{ -1e12 };
		double cooldownMs{ 1000 };
	};

	struct TreadmillDecision
	{
		bool warp{ false };
		Vec3 target;         // where to put Snake: the centre (keeps his height on the flat sandbox)
		Vec3 shift;          // target - root: what everything that travels with him (buddy) must move by
	};

	// Warp back to the centre once he is past the radius and still moving away from it (or past twice
	// the radius, whichever way he moves: e.g. pushed by something).
	TreadmillDecision Treadmill(const TreadmillInput& a_in);

	// ---- life bridge ----------------------------------------------------------------------------
	struct LifeResult
	{
		std::uint16_t life{ 0 };
		bool          died{ false };  // this hit would have killed him: kept at 1, report kGEvDied
	};
	// Elden Ring damage a_erDamage scaled into MGSV life points. Snake never reaches 0 here: Elden Ring
	// runs the death (YOU DIED, respawn), so MGSV must not run its own game over.
	LifeResult ApplyHurt(std::uint16_t a_life, float a_erDamage, float a_scale);
	// Our native life write (a_before -> a_written) did not stick when, 8 frames after it was applied, life (a_now) is back
	// nearer the value it replaced than ours. Writes under 400 points are not judged: MGSV's own regeneration (~13 a frame,
	// starting right after our write, which it never saw as a hit) could bring those back by itself.
	bool LifeWriteLost(int a_before, int a_written, int a_now);

	// MGSV's own regeneration after one of its hits (measured live 2026-10-13: an 11 m fall, 7801 -> 2651, sampled at 10 Hz):
	// life holds ~2.4 s, then regenerates at a rate that grows linearly to ~750 life/s over ~6.25 s and stays there (2651 ->
	// 7801 in 12.5 s). MGSV never sees Elden Ring's hits, so it regenerates from them at the full rate at once (~760 life/s:
	// 18 of Margit's hits survived in 70 s). The guest caps life after each Elden Ring hit with MGSV's curve: the most life
	// a_since seconds after a hit that left a_base. ([Combat] RegenDelaySec, RegenRampSec, RegenRate.)
	float RegenCap(float a_base, double a_since, float a_delay, float a_ramp, float a_rate);

	// MGSV-side feedback for an Elden Ring hit, played through MGSV's own Lua (GuestScript PR.Hurt):
	// Player.RequestToSetCameraNoise{levelX, levelY, time, decayRate} shakes MGSV's camera, which is
	// Elden Ring's camera (keys read from the exe: 0x1409DAE50), and Player.SetBloodToModel{pos, radius}
	// marks Snake where the hit came from (0x1409DD4E0). Stronger and longer for bigger shares of his life.
	struct HurtFeedback
	{
		bool  shake{ false };
		float level{ 0 };      // levelX = levelY
		float time{ 0 };       // s
		float decay{ 0 };      // decayRate
		bool  blood{ false };
		Vec3  bloodPos;        // MGSV world
		float bloodRadius{ 0 };
	};
	// a_kind is proto::HurtKind (3 = fall: shake only, no blood). a_from: unit direction toward where the
	// hit came from (MGSV world; zero if unknown).
	HurtFeedback FeedbackFor(std::uint16_t a_lifeLost, std::uint16_t a_lifeMax, std::uint32_t a_kind, const Vec3& a_root, const Vec3& a_from);

	// ---- bullet kinds -----------------------------------------------------------------------------
	// MGSV bulletId (TppEquip.BL_*) -> what Elden Ring should do with it (re-backlog M3). kNone: smoke,
	// flare, chaff, fulton.
	enum class BulletKind
	{
		kLethal,
		kTranq,
		kStun,
		kExplosive,
		kNone,
	};
	BulletKind KindOfBullet(std::uint32_t a_bulletId);

	// kGEvFire pad[1] (protocol v8): one body hit's lethal damage (the damage table's row [0] of the round's attack id) as a
	// share of a standard MGSV soldier's life (lifeParameterTable maxLife), in 1/10000; 0 = unknown or nothing lethal (the
	// host falls back to its per-class table). Over 10000 = more than one soldier's life (a sniper round: 11923).
	std::uint32_t DamageShare(std::uint32_t a_lethal, std::uint32_t a_soldierLife);

	// D-Walker's flamethrower, riding (docs/GUEST.md 18.3): where the stream starts and where it goes. MGSV aims it at the
	// camera's centre line, from the nozzle on the walker's right arm; a_nozzle is that point (walker root + facing), the
	// stream points from it at the spot a_reach metres along the camera's forward (so the first-person reticle and the
	// third-person aim both meet it). A degenerate aim falls back to the camera's forward.
	Vec3 FlameNozzle(const Vec3& a_root, float a_yaw, float a_ahead, float a_right, float a_up);
	Vec3 FlameDirection(const Vec3& a_nozzle, const Vec3& a_camPos, const Vec3& a_camForward, float a_reach);

	// ---- blasts -------------------------------------------------------------------------------------
	// MGSV attack id (TppDamage.ATK_*, read live from the game's own table) of a blast that reached MGSV's attack-area
	// system (Blast.cpp) -> what to send Elden Ring.
	//  kLethal/kTranq/kStun/kNone: Snake's thrown or placed explosives (grenades, sleep/stun/smoke grenades, molotovs,
	//    C4, claymores, mines) -> kGEvExplosion with that kind (kNone: smoke/flares, no damage).
	//  kRound: the blast of a round a gun fired (rockets, grenade launchers, missiles). That round already went out as
	//    kGEvFire with the explosive bit and Elden Ring blasts where it hits in Elden Ring's world: never twice.
	//  kIgnore: things of MGSV's own world (fuel drums, vehicles, helicopters) and anything unknown.
	enum class BlastKind
	{
		kLethal,
		kTranq,
		kStun,
		kNone,
		kRound,
		kIgnore,
	};
	BlastKind KindOfAttack(std::uint32_t a_attackId);
	const char* BlastKindName(BlastKind a_kind);

	// ---- Snake's explosives by TppEquip equip id (docs/GUEST.md 18.8) ----------------------------------------------
	// Thrown ones leave through the Throwing System (kGEvThrow), placed ones through the Placed System (kGEvPlaced);
	// kind = what their blast does (the same as KindOfAttack of the attack id their blast carries). Magazines, decoys,
	// bait, cages and wormhole devices aren't explosives (kNone).
	enum class ExplosiveUse
	{
		kNone,
		kThrown,
		kPlaced,
	};
	struct Explosive
	{
		ExplosiveUse  use{ ExplosiveUse::kNone };
		BlastKind     kind{ BlastKind::kIgnore };
		bool          impact{ false };    // thrown: explodes where it hits (molotov), no fuse
		std::uint32_t triggerMm{ 0 };     // placed: an enemy this close sets it off (0 = remote only: C4). UNVERIFIED radii
		bool          ownBlast{ true };   // placed: set off, it blasts through the attack-area system (the EMN mine casts a net)
		float         radius{ 0.0f };     // placed without its own blast: the radius of the explosion we report (m)
	};
	Explosive ExplosiveOf(std::uint16_t a_equipId);

	// The release velocity the first three points of MGSV's arc preview imply: equal time steps in free flight under
	// a_gravity (m/s^2, down). Nothing when they don't fit that (a point already past an obstacle, a degenerate arc).
	std::optional<Vec3> ArcVelocity(const Vec3& a_p0, const Vec3& a_p1, const Vec3& a_p2, float a_gravity = 9.8f);

	// Counts rising edges of a flag (the host's kHostPuppeting): the host forgets every placed explosive when its puppet
	// stops (a death, a load, an MGSV stall over the heartbeat timeout) without re-attaching, so each new start of the
	// puppet is a new audience that must hear about Snake's mines again.
	class RisingEdges
	{
	public:
		std::uint32_t Update(bool a_on)
		{
			count_ += a_on && !last_ ? 1u : 0u;
			last_ = a_on;
			return count_;
		}

	private:
		bool          last_{ false };
		std::uint32_t count_{ 0 };
	};

	// ---- shots from ammo counters ---------------------------------------------------------------
	// vars.ammoInWeapons drops by one per round; a weapon change or a reload never counts.
	class AmmoWatch
	{
	public:
		static constexpr int kSlots = 6;
		struct Shot
		{
			int           slot;
			std::uint16_t weapon;
			int           rounds;
		};
		void Reset() { primed_ = false; }
		// Returns the slot that fired this frame (the one with the biggest drop), if any.
		std::optional<Shot> Update(const std::array<std::uint16_t, kSlots>& a_weapons, const std::array<std::uint16_t, kSlots>& a_ammo);

	private:
		std::array<std::uint16_t, kSlots> weapons_{};
		std::array<std::uint16_t, kSlots> ammo_{};
		bool                              primed_{ false };
	};

	// One counter (D-Walker's gun): rounds fired since last frame, 0 on reload/refill.
	int CounterDrop(std::uint16_t a_prev, std::uint16_t a_now, int a_maxPerFrame = 32);

	// The HUD's main UI target size only ever grows within a stretch of frames, so a smaller game
	// resolution (MGSV's own Graphics options) or a one-off larger atlas would latch it for good. Per
	// present: did a UI draw match the latched size, were UI draws skipped for their size? True = forget
	// the latched size (a_presents presents in a row skipped draws and matched none).
	class MainSizeDecay
	{
	public:
		static constexpr int kPresents = 60;
		bool Present(bool a_matched, bool a_skippedForSize, int a_presents = kPresents);

	private:
		int misses_{ 0 };
	};

	// D-Walker's flamethrower fires no rounds (no ShotSolver call). Decided per burst of MGSV's GUN_FIRE,
	// not per frame: a gun leaves most frames without a new round between its rounds. A burst is flame
	// only if no buddy round arrived in all of it and it outlasted any gun's round interval; then one
	// event every kEveryMs.
	class FlameBurst
	{
	public:
		// vars.buddyGearMainWPType of D-Walker's flamethrower: VERIFIED 4 live (docs/GUEST.md 18.3, the
		// save whose walker burns). Any other main weapon is a gun and never makes flame events, even if
		// its rounds come through a ShotSolver caller the hook does not count (re-backlog M5).
		static constexpr std::uint8_t kFlamethrowerWp = 4;
		// The burst's input: Snake rides the buddy, MGSV reports GUN_FIRE, the hook is in, and the
		// walker's main weapon is the flamethrower.
		static bool Firing(bool a_nativeShots, bool a_riding, bool a_gunFire, std::uint8_t a_mainWp)
		{
			return a_nativeShots && a_riding && a_gunFire && a_mainWp == kFlamethrowerWp;
		}
		static constexpr std::uint64_t kSettleMs = 300;
		static constexpr std::uint64_t kEveryMs = 125;
		// a_rounds: buddy rounds the ShotSolver hook attributed so far. True: emit one flame event now.
		bool Update(bool a_firing, std::uint64_t a_rounds, std::uint64_t a_nowMs);

	private:
		bool          firing_{ false };
		bool          gun_{ false };
		bool          emitted_{ false };
		std::uint64_t start_{ 0 };
		std::uint64_t rounds_{ 0 };
		std::uint64_t last_{ 0 };
	};

	// The refill's magazine sizes (Ammo.cpp): PR.Magazines() answers "equip/magazine/sub,equip/magazine/sub,equip/magazine/sub"
	// for weapon slots 0-2. False on anything else.
	struct SlotMagazine
	{
		std::uint32_t equip{ 0 }, rounds{ 0 }, sub{ 0 };
	};
	bool ParseMagazines(std::string_view a_text, std::array<SlotMagazine, 3>& a_out);
}
