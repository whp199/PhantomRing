#pragma once

// Motion v1 (DESIGN.md §5.1): a host-side kinematic character controller. Snake's intended
// displacement (odometer delta mapped into ER) is resolved against Elden Ring's world with ER's own
// ray cast: wall stop + slide, step-up, ground snap, gravity fall. The result is applied to the
// puppet with ChrIns::SetPosition. Pure: the ray cast is a callback, so tests/test_host.cpp can run
// it against a synthetic world.
//
// Target (RE item E1) is to feed the motion into ER's Havok character proxy instead; this stays as
// the fallback.

#include "Math.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace phantom::er
{
	// Cast from a_from along a_delta (the whole segment). On a hit, a_hit receives the hit point.
	using RayFn = std::function<bool(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit)>;

	struct KinematicParams
	{
		float radius{ 0.35f };       // body radius kept from walls (m)
		float stepUp{ 0.5f };        // highest ledge walked onto without a jump (m)
		float snapDown{ 0.6f };      // how far down the feet follow the ground while grounded (m)
		float chestHeight{ 1.3f };   // a wall ray (m above the feet)
		float waistHeight{ 0.9f };   // ... and two more: a cart's bed or a table between knee and chest stops the body too
		float headHeight{ 1.6f };    // (session 4: Snake walked into a wooden cart between the knee and chest rays and got stuck in it)
		float gravity{ 9.81f };      // m/s^2
		float terminalSpeed{ 50.0f };
		float slideProbe{ 0.2f };    // lateral offset of the two rays that estimate a wall's normal (m)
		// Snake's height in his stance (MGSV's: standing ~1.8 m, crouched ~1.2, prone ~0.5; riding: the mount's): the wall rays
		// above it are left out (a crouched Snake goes under a 1.3 m beam) and a move never takes him under a ceiling lower than
		// it (player report 2026-10-14: Snake walked into the low space under the Roundtable Hold's basement stairs and was stuck
		// there). 0 = no ceiling check, every wall ray.
		float bodyHeight{ 0.0f };
		// The ground under the body is found by five rays, its centre and four points at kGroundProbeRadius of its radius (BodyGround),
		// not one under its centre: Elden Ring's wooden floors are planks with gaps, and a single ray through a gap read "no ground"
		// (live 2026-10-14: Snake fell through the wooden scaffolding on Stormveil's side the Tarnished had just climbed). Standing,
		// walking, falling, the start and the landing of a jump or a hop all use it. false = the centre ray alone (the old way;
		// [Puppet] ground_probes).
		bool groundProbes{ true };
		// A jump in the air (KinematicJump, protocol v14 kGuestJumping): the knee ray drops to jumpAirStep above the feet, and a
		// ledge's top up to that far above them is landed on (Elden Ring's capsule catches an edge with its round bottom); the
		// horizontal velocity of the takeoff carries on, steered toward Snake's own motion at jumpAirControl m/s^2 at most.
		float jumpAirStep{ 0.3f };
		float jumpAirControl{ 3.0f };
	};
	// A plain fall's ledge catch: in the air (not a hop or a jump) the knee ray sits this + 0.05 m above the feet, and a ledge top
	// that far above them is landed on -- a higher one is a wall (KinematicStep).
	inline constexpr float kFallStep = 0.05f;
	inline constexpr float kSeamProbe = 0.07f;  // wider than the widest seam seen (10 cm across both sides), well inside the body

	// Snake's height for a jump's head room: his stance's bodyHeight, or (no ceiling check) the head ray's height + 0.2 m.
	inline float JumpBodyHeight(const KinematicParams& a_p) { return a_p.bodyHeight > 0.0f ? a_p.bodyHeight : a_p.headHeight + 0.2f; }

	// The four side probes of BodyGround sit at this share of the body radius: inside the body, clear of a wall it stands against
	// (walls are kept a full radius away).
	inline constexpr float kGroundProbeRadius = 0.8f;
	// A probe's ground counts as the body's when it is the centre's, or a second probe found ground no more than this below it:
	// one probe on a step's edge or a shaft's rim is not standing there (a capsule with its centre past an edge slides off it).
	inline constexpr float kGroundSupportTolerance = 0.1f;

	// Two side probes' means (opposite pairs) that differ by more than this are not one plane (a step inside the floor).
	inline constexpr float kGroundPlaneTolerance = 0.03f;

	struct GroundProbe
	{
		bool  supported{ false };  // the body stands on something (the centre's ground, or two probes')
		float y{ NAN };            // the ground under the body's axis (below)
		int   hits{ 0 };           // probes that found any ground (0-5)
		bool  centre{ false };     // the centre ray found ground
		int   level{ 0 };          // probes (the centre too) whose ground is within kGroundSupportTolerance of y (0-5)
	};

	// The ground under a body standing at a_feet: five rays down from a_from above the feet, a_length long (the centre's, and with
	// a_probes four more at kGroundProbeRadius x a_radius, a cross turned 30 degrees from x/z so planks laid along one direction
	// see five different offsets across their gaps). A side ray that starts inside something (a hit at its very origin) finds
	// nothing. Where the body stands (y):
	//  - the centre found ground: that ground (a slope, a step's edge, a floor: the old single ray, no hover on a slope -- review
	//    2026-10-14: the highest probe floated him 6-25 cm over every 10-40 degree slope). Unless three or more side probes stand on
	//    ground above it (a slope or a step's edge lifts two at most): a gap between planks under the centre, and the floor is the
	//    plane those side probes agree on (opposite probes' mean: on any plane that is its height under the axis);
	//  - the centre found nothing: the side probes' plane (both probes of an opposite pair found ground), or failing that the
	//    highest side probe that a second probe holds (another found ground at most kGroundSupportTolerance below it). One probe
	//    on a step's edge or a shaft's rim is no support: a capsule with its centre past an edge slides off it.
	GroundProbe BodyGround(const Vec3& a_feet, float a_from, float a_length, float a_radius, const RayFn& a_ray, bool a_probes = true);

	// The puppet's start stands only on firmer ground than a walk keeps (SupportedStart): the centre's own ground, or four of
	// the five probes at the body's level. Two side probes on a shaft's rim hold a walking body (it slides off once fewer
	// do), but a start there leaves Snake centimetres from the drop (review 2026-10-14: 4 cm from Fort Haight's 19 m shaft).
	inline bool StartGround(const GroundProbe& a_g) { return a_g.supported && (a_g.centre || a_g.level >= 4); }

	// Snake's body rays (er/Puppet.cpp BodyRay): a ray mostly up or down (ground, landing, head room) goes to a_ground; the others
	// (walls) to a_wall and, when a_wallAlso is set, to it too, the nearer hit kept -- the walls both filters see (review
	// 2026-10-14: the ground filter found the scaffold's floor in the characters-only layers the wall filter misses, so the
	// railings around it were likely missed by the walls).
	RayFn BodyRays(RayFn a_ground, RayFn a_wall, RayFn a_wallAlso);

	struct KinematicState
	{
		Vec3  pos;                // feet (Havok space)
		float vy{ 0 };            // vertical velocity (m/s, up positive)
		bool  grounded{ false };
		float groundY{ NAN };     // last ground height found under the feet
		// The ceiling check's way out (kCeilingEscapeS): already under a ceiling lower than him and refused that long in a row,
		// the check stands aside until he stands free again -- the way out from under a staircase can dip (a stringer lower
		// than the treads around it; live 2026-10-14 a crouched Snake stuck under the Roundtable Hold's basement stairs: every
		// move out read "lower than here").
		float underRefusedS{ 0 };
		bool  ceilingEscape{ false };
		// A hop (KinematicHop: MGSV's dive, roadmap 2.9) in the air: the move is hopVel (horizontal, m/s) instead of what Snake
		// asks, the low wall ray sits hopStep above the feet (in the air a ledge is a wall until the feet are nearly over it, so
		// a 1 m wall is never climbed), a ledge top within hopStep above the feet is landed on, and nothing above hopHead (his
		// height at the take-off) may come lower than his head. Ends on landing, or after hopLeftS (then it is an ordinary fall).
		bool  hopping{ false };
		Vec3  hopVel;
		float hopStep{ 0 };
		float hopHead{ 0 };
		float hopLeftS{ 0 };
		// Seconds since he was last on the ground (0 while grounded): a dive pressed at a rim reaches the host a few frames after
		// his centre passed it (HopParams::graceS).
		float airS{ 0 };
		// A jump (KinematicJump) until it lands: its horizontal velocity (m/s), steered in the air. jumpFromY: the height a jump
		// or a hop left from -- its landing counts as a fall only from there down (fall damage comes from real heights, never
		// from its own rise).
		bool  jumping{ false };
		Vec3  airVel;
		float jumpFromY{ 0 };
	};
	inline constexpr float kCeilingEscapeS = 0.75f;

	struct KinematicResult
	{
		Vec3  moved;               // horizontal displacement actually applied
		bool  blocked{ false };    // a wall clipped the wanted move
		bool  slid{ false };       // the remainder was redirected along the wall
		bool  stepped{ false };    // the feet went up a ledge this frame
		bool  landed{ false };     // touched ground after falling
		float landingSpeed{ 0 };   // m/s downward at landing
		Vec3  wallPoint;           // blocked: where the body met the wall (knee height), e.g. a crate to break
		bool  ceiling{ false };    // the move was refused: it went under a ceiling lower than bodyHeight (and lower than here)
		bool  ceilingEscape{ false };  // this frame the check stood aside for good (KinematicState::ceilingEscape began)
		float headroom{ INFINITY };  // with bodyHeight: the room above the feet where the move ended, or would have (refused) (m)
		bool  jumpLanded{ false };   // landed is a jump's landing (landingSpeed is then that of a fall from the takeoff's height)
		bool  headBump{ false };     // a jump's or a hop's rise met a ceiling: it stopped there
	};

	// A jump's start (KinematicJump). ok = false: nothing changed (why says what refused it).
	struct JumpStart
	{
		bool        ok{ false };
		float       apex{ 0 };        // how high the feet will rise (m): the wanted apex, or less under a ceiling
		float       room{ INFINITY }; // the room above his head before the jump (m)
		float       speed{ 0 };       // horizontal takeoff speed (m/s)
		const char* why{ "" };
	};
	// Snake jumps (Elden Ring-like: protocol v14 kGuestJumping): on the ground only (not in a hop), with the room above his head
	// (JumpBodyHeight) for at least a_minRoom; the rise is a_apex (m) or what the ceiling leaves of it. a_runVel: his horizontal
	// velocity (m/s, capped at a_maxSpeed), which carries on in the air. KinematicStep then flies him until he lands.
	JumpStart KinematicJump(KinematicState& a_state, const Vec3& a_runVel, float a_apex, float a_minRoom, float a_maxSpeed, const KinematicParams& a_params,
		const RayFn& a_ray);

	// Elden Ring moved its world origin by a_delta: the state's heights and place move with it (a jump's takeoff height too: its
	// landing's fall damage is the drop from there).
	void KinematicShift(KinematicState& a_state, const Vec3& a_delta);

	// The guest's kGuestJumping as jumps (Puppet.cpp): a jump starts on its rising edge. The flag is remembered on every frame
	// the guest's state was read, ready or not; a jump starts only while the guest reads ready. (A frame that read not ready
	// once forgot the flag, and the flag still up from the same press -- MGSV up to 2 s, Elden Ring lands in ~1 -- read as a
	// second press: a jump nobody pressed.)
	class JumpEdge
	{
	public:
		// One frame: a_read = the guest's state was read this frame; a_ready = it reads ready; a_flag = its kGuestJumping.
		bool Update(bool a_read, bool a_ready, bool a_flag)
		{
			if (!a_read) {
				return false;  // nothing read: the last flag stands
			}
			const bool edge = a_flag && !last_;
			last_ = a_flag;
			return edge && a_ready;
		}
		void Reset(bool a_flag) { last_ = a_flag; }

	private:
		bool last_{ false };
	};

	// MGSV's dive as a hop (roadmap 2.9; the jump is R3: KinematicJump). A parabola height m high and length m long on level
	// ground, tunable; the hop never takes his head (minRise: at least this much room above it, else no hop) into a ceiling. In
	// the air it is a jump's (KinematicStep): the same ledge catch, head check and landing on the whole body's ground.
	struct HopParams
	{
		float height{ 0.6f };    // apex above the take-off (m): also the highest ledge it lands on (plus step)
		float length{ 2.5f };    // take-off to landing on level ground (m)
		float step{ 0.15f };     // in the air, a ledge top this far above the feet is landed on (m)
		float minRise{ 0.15f };  // less room than this above his head at the take-off: no hop
		// His height at the take-off (m; 0 = a_params.bodyHeight, or headHeight with none). The puppet passes his standing height:
		// the guest only arms a dive on X while STAND, but the frame that confirms it may already say prone, and a prone 0.5 m
		// would let a full hop take him under a 1.9 m beam (review of 2.9).
		float head{ 0.0f };
		// Off a rim: the dive's flag needs a few frames (X, the guest's sign, the shared memory) and MGSV's dive moves him on at
		// ~5 m/s meanwhile, so his centre is often past the rim when the hop comes. Left the ground less than graceS ago and at
		// most graceDrop below it, he still hops -- from that ground's height.
		float graceS{ 0.2f };
		float graceDrop{ 0.3f };
	};

	// Starts a hop along a_dir (horizontal, unit) from where a_state stands: grounded (or just off the ground: graceS), not
	// already hopping or jumping. Returns the rise granted (m; less than a_hop.height under a ceiling), 0 when refused (in the air, or no
	// room above his head); refused, a_state is untouched.
	float KinematicHop(KinematicState& a_state, const Vec3& a_dir, const HopParams& a_hop, const KinematicParams& a_params, const RayFn& a_ray);

	// One frame. a_want: wanted horizontal displacement (y ignored; in a jump it steers, in a hop it is ignored). a_dt: seconds.
	KinematicResult KinematicStep(KinematicState& a_state, const Vec3& a_want, float a_dt, const KinematicParams& a_params, const RayFn& a_ray);

	// How far (<= a_maxDist) the body can go along a_dir (unit, horizontal) from a_feet before its
	// radius touches a wall, probing at knee (just above a step), waist, chest and head height. a_normal receives
	// the wall's horizontal normal when one was hit (estimated from two side rays).
	float WallClearance(const Vec3& a_feet, const Vec3& a_dir, float a_maxDist, const KinematicParams& a_params, const RayFn& a_ray, Vec3* a_normal);

	// The room above a_feet (m, from the feet to the first thing above them within a_max; INFINITY when nothing is): one ray up
	// from 0.25 m above the feet (on a step's edge or a slope it never meets the ground he stands on).
	float Headroom(const Vec3& a_feet, float a_max, const RayFn& a_ray);
	// Headroom that a seam can't fool: when the ray up at a_feet finds nothing, four more 7 cm around it (along x and z) decide.
	// Elden Ring's stairs are tread plates with 4-10 cm seams between them (live 2026-10-14, the Roundtable Hold's basement
	// stairs): a ray through a seam read "no ceiling" and a crouched Snake crept under them seam by seam to a 0.57 m ceiling.
	// a_stepUp > 0: each probe measures from its own ground when that is up to a_stepUp higher than a_feet (a step's top is the
	// floor there, not a ceiling: live 2026-10-14, Stormveil's side stairs read "a ceiling 0.26 m above").
	float SolidHeadroom(const Vec3& a_feet, float a_max, const RayFn& a_ray, float a_stepUp = 0.0f);

	// Where Snake can start standing (the puppet's start, kinematic): nullopt when the body stands on ground at a_feet (BodyGround
	// within snap_down, StartGround: the centre's ground, or four of five probes -- planks with gaps under the centre are ground),
	// else the nearest point within 1.2 m where the body stands on ground (its centre and four points at its radius, within
	// step_up of the feet; failing that at a distance, all five of BodyGround's probes at its level) and that it can reach without a wall --
	// nullopt again when there is none (a real drop: he falls). Live: off a ladder at the top of Fort Haight's tower Elden Ring's
	// capsule stood on the shaft's rim with its centre over the hole, and Snake fell 19 m down it (two probes on a rim hold a
	// walking body, not a start).
	std::optional<Vec3> SupportedStart(const Vec3& a_feet, const KinematicParams& a_params, const RayFn& a_ray);

	// The ground under a_feet, for the log (Puppet.cpp: why a fall began): BodyGround's five rays (a body of a_radius) down from
	// a_up above the feet, a_down long. a_out[i] = the hit's height below the feet (m, < 0 above), NAN = none; the centre first.
	void GroundProbes(const Vec3& a_feet, float a_up, float a_down, float a_radius, const RayFn& a_ray, float (&a_out)[5]);

	// How boxed in the body is: of 8 horizontal directions, how many meet geometry within a_reach of the body's axis at knee
	// or chest height (a flat wall: ~3, a corner ~4-5, inside a cart or a rock: 7-8).
	int Enclosed(const Vec3& a_feet, float a_reach, const KinematicParams& a_params, const RayFn& a_ray);

	// A fall that never lands (roadmap 1.3, pure): with NoDead set, a chasm whose death is Elden Ring's kill volume (not a
	// landing) can't kill the puppet, and kinematic gravity would let Snake fall for ever. Update() is true once, the frame
	// a fall has lasted a_seconds or gone a_drop metres below where it began (0 = that check off); a landing re-arms it.
	class FallWatch
	{
	public:
		bool                Update(bool a_grounded, float a_feetY, float a_dt, float a_seconds, float a_drop);
		void                Reset() { *this = FallWatch(); }
		void                Shift(float a_dy) { startY_ += a_dy; }  // Elden Ring moved its world origin
		[[nodiscard]] float Seconds() const { return seconds_; }
		[[nodiscard]] float Drop() const { return drop_; }

	private:
		bool  falling_{ false };
		bool  fired_{ false };
		float seconds_{ 0 };
		float startY_{ 0 };
		float drop_{ 0 };
	};

	// Unsticking (pure): a puppet boxed in (Enclosed >= 7) for a_seconds goes back to the last place it stood free. And a stall
	// (Stall): Snake asked to move and was refused (a wall, a ceiling) in every direction he tried -- at least three of eight
	// sectors -- for a_seconds of trying (pauses don't count, up to 4 s each) also goes back. Live 2026-10-14 (the player's
	// session, under the Roundtable Hold's basement stairs): 59 wall contacts over 1353 refused frames and never boxed in 7 of 8:
	// the old check alone never fired.
	class Unstick
	{
	public:
		// Each frame: where the feet are, how boxed in, grounded, now (s); a_headroom: there is room for his height here (a
		// place under a low ceiling is never a "free" place to go back to). Returns the place to go back to, or nothing.
		std::optional<Vec3> Update(const Vec3& a_feet, int a_enclosed, bool a_grounded, double a_nowS, float a_seconds, bool a_headroom = true);
		// Each kinematic frame: what Snake asked (a_want, horizontal) and what the controller moved. A free place to go back to
		// (or nothing: a_noPlace then says the stall fired with none -- the caller hints at Tarnished mode).
		std::optional<Vec3> Stall(const Vec3& a_feet, const Vec3& a_want, const Vec3& a_moved, bool a_grounded, double a_nowS, float a_seconds,
			bool* a_noPlace = nullptr);
		void                Reset() { *this = Unstick(); }
		void                Shift(const Vec3& a_delta);  // Elden Ring moved its world origin
		[[nodiscard]] std::uint64_t Count() const { return count_; }
		[[nodiscard]] std::uint64_t Stalls() const { return stalls_; }   // stalls that fired (moved back or hinted)
		[[nodiscard]] float         StalledSeconds() const { return stalledS_; }

	private:
		std::optional<Vec3> Back(const Vec3& a_feet) const;
		Vec3          free_[8];
		int           freeCount_{ 0 };
		double        lastFreeS_{ -1.0 };
		double        boxedSinceS_{ -1.0 };
		std::uint64_t count_{ 0 };
		// stall
		float         stalledS_{ 0 };     // seconds of refused asking in this stall
		float         freeRunS_{ 0 };     // seconds of real moving since the stall's last refusal
		double        lastStallS_{ -1.0 };  // the last frame of this stall (refused or idle)
		std::uint8_t  sectors_{ 0 };      // directions tried (8 sectors)
		std::uint64_t stalls_{ 0 };
	};
}
