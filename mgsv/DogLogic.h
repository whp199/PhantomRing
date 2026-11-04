#pragma once
// D-Dog as a battle buddy in Elden Ring, the pure half (docs/research/ddog.md §3.5-3.7; the game glue in Dog.cpp, tests in
// tests/test_guest_dog.cpp). No game, Windows or mapping access here.
//
//  - Its style: MGSV's style byte (the dog system's +0xD4, set by the dog's equipment) -> the protocol's DogStyle, and the TppDamage
//    attack id of each style's attack (ATK_DDogBite 144 / Knife 145 / StunKnife 146).
//  - The attack job: the player's "Keep 'em busy" / "Do it" / "Get it" carried out on the Elden Ring enemy the host picks
//    (kActorDogTarget): a dedicated inert MGSV soldier stands at him (ActorProxy), MGSV's own order sends the dog, and the soldier's
//    dog-attack handler accepting the bite is the "connected" (kGEvDogAttack Ordered -> Connected -> Released, or Refused / Lost).
//  - The dog's place: moved with Snake's treadmill jumps through its location object (no ~6 m jump in Elden Ring; Wait keeps its
//    place) and, far behind and out of view, put beside him (the leash, D-Walker's catch-up rule).

#include "Logic.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <optional>

namespace phantom::guest::dog::plan
{
	using logic::Vec3;

	// ---- style ------------------------------------------------------------------------------------------------------------
	// MGSV's style byte (the dog system's +0xD4) -> DogStyle. The dog's attack order (0x140C57966) takes 1 as a fetch and 0, 2-7 as
	// attacks; its attack state (0x140C64DE0) sends a soldier the knife message for modes 2 and 4 (its byte +0xA set only for 2: the
	// stun flag), 0x48E4EDE3DAED for 3 and the bite for any other, so 5-7 are bites ("busy"). The byte is vars.buddyDogEquipType: live
	// with MGSV's own CHANGE EQUIPMENT (2026-11-04, both games) NAKED 0 -> command 11 "Keep 'em busy", SNEAKING (STUN) 2 -> command 13
	// "Do it (knock out enemy)", BATTLE DRESS 3 -> 14 "Do it (wound enemy)", SNEAKING (KNIFE) 4 -> 12 "Do it (kill enemy)", TACTICAL
	// (FULTON) 1 -> 15 "Get it" (the 2026-11-03 table had 2 and 4 swapped: it read the soldier's +0xA byte as "lethal"). The protocol's
	// DogStyle numbers are semantic (kDogKill 2, kDogKnockOut 3, kDogWound 4). Negative = unknown (the dog system not learned yet).
	proto::DogStyle StyleOf(int a_byte);
	// The style the player's call-menu command names (MGSV's menu labels its attack entry by the dog's equipment and gives that
	// entry's command): 11 busy, 12 kill, 13 knock out, 14 wound, 15 fetch; nullopt for any other. The truth for a player's order.
	std::optional<proto::DogStyle> StyleOfCommand(int a_command);
	std::optional<int>             CommandOfStyle(proto::DogStyle a_style);  // its inverse (fetch 15, ...); nullopt for unknown
	// The attack id of a style's attack: busy 144 (ATK_DDogBite), kill 145 (ATK_DDogKnife), knock out 146 (ATK_DDogStunKnife), wound
	// 147 (ATK_DDogBodyAttack), fetch / unknown 0.
	std::uint32_t AttackIdOf(proto::DogStyle a_style);
	// The attack id of the message the dog sent the soldier: 0x887890EBE04A bite 144, 0x53E70021556A the knife (145; 146 with the
	// message's stun flag, +0xA: the stun knife), 0x48E4EDE3DAED the body attack 147; 0 for anything else.
	std::uint32_t AttackIdOfMessage(std::uint64_t a_message, bool a_stun = false);
	const char*   StyleName(proto::DogStyle a_style);
	const char*   PhaseName(proto::DogPhase a_phase);

	// The buddy commands (docs/GUEST.md 18.3): 8 whistle, 9 wait, 10 bark, 11-15 the attack / fetch (one message: the equipment
	// decides), 16 DD.
	inline constexpr int kCmdWhistle = 8, kCmdWait = 9, kCmdBark = 10, kCmdAttackFirst = 11, kCmdAttackLast = 15, kCmdTalk = 16;
	inline constexpr bool IsAttackCommand(int a_cmd) { return a_cmd >= kCmdAttackFirst && a_cmd <= kCmdAttackLast; }
	inline constexpr bool IsDogCommand(int a_cmd) { return a_cmd >= kCmdWhistle && a_cmd <= kCmdTalk; }
	// The player's own command that leaves the dog where MGSV puts it (Wait, DD): no whistle of ours after it.
	inline constexpr bool PlayerPlacesDog(int a_cmd) { return a_cmd == kCmdWait || a_cmd == kCmdTalk; }

	// ---- the attack job (§3.5) ----------------------------------------------------------------------------------------------
	struct JobSettings
	{
		std::uint64_t realizeTimeoutMs{ 1500 };  // the target's soldier realized and placed within this, else Refused
		std::uint64_t reachTimeoutMs{ 12000 };   // ordered, not connected within this: Lost
		std::uint64_t busyMaxMs{ 10000 };        // connected this long: Released
		std::uint64_t stuckGraceMs{ 4000 };      // MGSV's dog takes 0.7-1.5 s to set off after an order, and stalks with the knife: no
		std::uint64_t stuckMs{ 5000 };           // stuck check before this; then its distance not shrinking by stuckGain for this long:
		                                         // Lost (a backstop: MGSV dropping the order is the usual verdict, orderDropped)
		float         stuckGain{ 0.5f };
		float         stuckNear{ 2.5f };         // ... unless it is this close already (it leaps from there)
		std::uint64_t contactGraceMs{ 400 };     // the dog's attack state ending this soon after the accept is the same bite settling
		// "Get it": the run, then MGSV's Fulton beside the sleeper (its balloon takes him ~8 s after it is attached): Connected only at
		// that yank (kGEvFulton), so its reach time is longer, and a dog standing within fetchNear of him is busy, not stuck (live:
		// 16 s from the order to the yank for a sleeper 27 m away; a 14 m one read as stuck 3.5 s before its balloon took him).
		std::uint64_t fetchTimeoutMs{ 30000 };
		float         fetchNear{ 6.0f };
		std::uint64_t dropLostMs{ 500 };         // an attack order MGSV let go of this long without a bite: Lost
		std::uint64_t fetchDropLostMs{ 8000 };   // a fetch: MGSV lets go of the sleeper ~3 s before its balloon's yank (live)
	};

	enum class Stage : std::uint8_t
	{
		kIdle,
		kRealize,    // the target's dedicated soldier is being enabled and placed
		kOrdered,    // MGSV's order went out: the dog runs
		kConnected,  // the soldier accepted the bite / knife (or the fetch's balloon took him)
	};
	const char* StageName(Stage a_stage);

	struct JobInput
	{
		std::uint64_t nowMs{ 0 };
		bool          targetPresent{ false };  // the job's handle is in the ActorTable, hostile and alive
		bool          proxyReady{ false };     // its dedicated soldier stands at it (realized, placed): the order can go
		bool          accepted{ false };       // the soldier accepted the dog's attack since the order (the soldier-side hook)
		bool          fultoned{ false };       // MGSV's balloon took the target's proxy (a fetch's end)
		bool          contactOver{ false };    // the dog let go: its attack state ended (or the contact bit fell) after the accept
		float         dogDistance{ -1.0f };    // the dog's feet to the proxy's (m, horizontal; < 0 unknown)
		bool          cancel{ false };         // the player whistled / ordered Wait / DD meanwhile, or the dog went away
		int           cancelCommand{ 0 };      // ...which command (8 whistle, 9 Wait, 16 DD; 0 unknown): after Wait or DD the job's end
		                                       // sends no whistle of ours (it would undo the player's Wait at once: review 0.8.0)
		std::uint64_t orderDroppedMs{ 0 };     // for how long MGSV's order record no longer targets the soldier it held after the order
		                                       // (0: it still does / unknown). An attack: MGSV gave the order up (unreachable, the target
		                                       // check failed; live, a 2 s distance rule had called two stalking knife runs stuck that
		                                       // MGSV still carried out). A fetch: the dog has fixed its balloon -- the yank follows
	};

	struct JobStep
	{
		std::optional<proto::DogPhase> event;  // kGEvDogAttack to send now
		bool                           dedicate{ false };  // stand a soldier at the handle (ActorProxy Dedicate)
		bool                           order{ false };     // issue MGSV's attack command on that soldier now
		bool                           release{ false };   // give the soldier back (ActorProxy Release)
		bool                           whistle{ false };   // call the dog back (WhistleBack)
		const char*                    why{ "" };
	};

	class Job
	{
	public:
		// The player's attack order. a_pick = the host's kActorDogTarget handle (0: none -> Refused at once, handle 0, whistle -- not
		// when a_waiting: a dog the player left waiting stays where it waits, MGSV never got the order).
		JobStep Start(std::uint64_t a_pick, proto::DogStyle a_style, std::uint64_t a_nowMs, bool a_waiting = false);
		JobStep Update(const JobInput& a_in, const JobSettings& a_s);
		// Ends a running job at once (the layer turned off, the dog gone): Released if it had connected, else Lost (Refused before the
		// order); nothing when idle.
		JobStep Abort(const char* a_why);

		[[nodiscard]] Stage           GetStage() const { return m_stage; }
		[[nodiscard]] bool            Busy() const { return m_stage != Stage::kIdle; }
		[[nodiscard]] bool            Engaged() const { return m_stage == Stage::kConnected; }
		[[nodiscard]] std::uint64_t   Handle() const { return m_handle; }
		[[nodiscard]] proto::DogStyle Style() const { return m_style; }
		[[nodiscard]] std::uint64_t   OrderedMs() const { return m_orderedMs; }
		[[nodiscard]] std::uint64_t   StartedMs() const { return m_startedMs; }
		[[nodiscard]] std::uint64_t   ConnectedMs() const { return m_connectedMs; }

	private:
		JobStep End(proto::DogPhase a_phase, bool a_release, const char* a_why);

		Stage           m_stage{ Stage::kIdle };
		std::uint64_t   m_handle{ 0 };
		proto::DogStyle m_style{ proto::kDogStyleUnknown };
		std::uint64_t   m_startedMs{ 0 }, m_orderedMs{ 0 }, m_connectedMs{ 0 };
		float           m_best{ -1.0f };     // the nearest the dog came to its target since the order
		std::uint64_t   m_bestMs{ 0 };
	};

	// ---- the dog's place (§3.7) ---------------------------------------------------------------------------------------------
	// Whether a treadmill jump moves the dog along (through its location object): only a located dog, within a_maxDistance of Snake
	// (its MGSV place stays inside the airport) -- a waiting one within a_waitMax (Wait keeps its place in Elden Ring terms as long
	// as possible), and one carrying out an order always (a jump must not move it 6 m relative to its prey).
	struct TreadmillInput
	{
		bool  enabled{ false };   // [Dog] Enabled + Treadmill
		bool  located{ false };   // its location object is confirmed
		bool  waiting{ false };   // the player ordered Wait
		bool  engaged{ false };   // an attack job runs
		float distance{ 0.0f };   // dog to Snake (m, horizontal)
		float maxDistance{ 40.0f };
		float waitMax{ 160.0f };
	};
	bool MoveAlong(const TreadmillInput& a_in);

	// MGSV's camera field as Follow.cpp's catch-up sees it (MGSV's camera is Elden Ring's): in front of the camera and inside ~35 deg
	// either side of the line to Snake, plus 2 m for the dog's own size.
	struct View
	{
		bool valid{ false };
		Vec3 cam;
		Vec3 dir;    // unit, horizontal: camera -> Snake
		float back{ 0.0f };  // camera to Snake (m, horizontal)
	};
	View ViewOf(const Vec3& a_cam, const Vec3& a_snake);
	bool InView(const View& a_view, const Vec3& a_p);

	// The leash: a free dog (no Wait, no order) more than a_leash from Snake that the camera can't see is put where it can't be seen
	// either: 2.5 m behind him and a_side m beside him (left of the view), else behind the camera along his motion. Nothing when it
	// is near enough, in view, or no hidden spot is nearer than it is.
	std::optional<Vec3> LeashSpot(const Vec3& a_snake, const Vec3& a_velocity, const View& a_view, const Vec3& a_dog, float a_leash, float a_side = -2.0f);

	// ---- what Elden Ring sees: the published place and the drawn dog (docs/research/ddog.md §9 "0.8.0 polish") ----------------
	// MGSV's own dog now and then moves more in one frame than any dog runs: its attack's lunge starts with a 0.5-0.6 m step (live
	// 2026-11-04, the dog's location object and its draws alike), and its running place updates every other frame (0.28-0.42 m, then
	// nothing). The place the guest publishes -- and the dog's draws, moved by the same offset -- follows the real one at most
	// a_maxSpeed (m/s) apart from it, in Elden Ring's terms (odometer space: Snake's treadmill jumps move the dog and Snake together,
	// so they are no motion here). Horizontal only (Elden Ring's ground decides the height). A step over a_teleport is MGSV's own
	// warp of its dog and is taken at once; so is anything after a_reset (our leash, a staging placement, a new odometer epoch).
	struct DisplaySettings
	{
		float maxSpeed{ 14.0f };   // m/s: D-Dog's sprint is ~11.6 m/s live
		float teleport{ 15.0f };   // m
	};
	class DisplayLimiter
	{
	public:
		// a_real: the dog's place now (odometer space); a_dt: seconds since the last frame. Returns the place to show.
		Vec3 Step(const Vec3& a_real, float a_dt, bool a_reset, const DisplaySettings& a_s = {});
		void Reset() { m_have = false; }
		[[nodiscard]] Vec3          Offset() const { return m_offset; }  // shown - real, horizontal (0 while it keeps up)
		[[nodiscard]] std::uint64_t Limited() const { return m_limited; }
		[[nodiscard]] std::uint64_t Teleports() const { return m_teleports; }
		[[nodiscard]] float         MaxLag() const { return m_maxLag; }

	private:
		bool          m_have{ false };
		Vec3          m_shown;
		Vec3          m_offset;
		std::uint64_t m_limited{ 0 }, m_teleports{ 0 };
		float         m_maxLag{ 0.0f };
	};

	// ---- After a Wait: the offset Elden Ring was shown of the waiting dog (WaitKeepsPlace) -----------------------------------------
	// Once the Wait ends (a whistle, an attack order) the offset goes, but with the dog's own way to its goal -- Snake, or the soldier
	// of an attack order -- not by the clock: at a fixed 4 m/s a dog ordered from Wait reached its enemy with metres of offset left and
	// bit beside him in Elden Ring (review 0.8.0). The offset is scaled by (way left - arrive) / (way at the start - arrive) (never back
	// up), plus a slow 0.5 m/s so none is left with a dog that follows a few metres behind; gone once the dog holds its enemy. A goal
	// change starts it over from the offset left; no goal yet (an order's soldier not there: the dog still sits) keeps it; no dog
	// place, or a dog already at its goal, falls back to 4 m/s.
	class WaitFade
	{
	public:
		enum Kind : std::uint8_t
		{
			kNone,     // no goal yet: the offset holds
			kSnake,    // a whistle or the Wait's end: the dog runs to Snake (arrives at 3 m)
			kSoldier,  // an attack order: the dog runs to its soldier (arrives at 1 m)
			kHolding,  // the dog holds its enemy: no offset
		};
		struct Goal
		{
			Kind kind{ kNone };
			Vec3 at;
		};
		Vec3 Step(const Vec3& a_offset, const std::optional<Vec3>& a_dog, const Goal& a_goal, float a_dt);

	private:
		bool  m_have{ false };
		Kind  m_kind{ kNone };
		Vec3  m_from;
		float m_way{ 0.0f };
		float m_frac{ 1.0f };
	};

	// ---- Snake staged at MGSV's ladder or ledge (Climb.h) ----------------------------------------------------------------------
	// While Snake climbs at real MGSV geometry (40-90 m from the sandbox) MGSV's dog runs after him, and then MGSV warps it to him
	// (live 2026-11-04: an 84 m jump in one frame, published as an 82 m jump in Elden Ring; back home, warped again 19 m from him).
	// The guest takes the dog along instead: once Snake's root shows at the site, a free dog is put a_behind metres behind him there
	// (his back is where he came from; the obstacle is in front), and when he is home again it is put back by the same move. The
	// published place stays the dog's place relative to Snake's home meanwhile (Frame.cpp subtracts the move). Where to put it.
	Vec3 StagedSpot(const Vec3& a_root, float a_yaw, float a_behind = 2.0f);
	// Snake has left home for the site (a placement of ours put him there), or is back.
	inline bool AtSite(const Vec3& a_root, const Vec3& a_home) { return (a_root - a_home).LengthXZ() > 3.0f; }

	// ---- MGSV's own markers while the dog carries out an order ---------------------------------------------------------------
	// MGSV marks the dog's target soldier (red, with its distance) and the dog (green) from the order to the let-go; in Elden Ring they
	// hang near the Elden Ring enemy (the soldier stands at his MGSV place) and over the dog, beside the host's own marks. Their UI
	// draws (live 2026-11-04, `!capture probe` mid-bite: the only draws a bite adds, 16 of them: icon + digits, each at full and at 0.6
	// alpha, plus a 0.3x shadow) carry their colour in the vertex shader's cVSLight[1].rgb: green (0.4, 0.9, 0.7), red (1.0, 0.4, 0.3)
	// -- the rest of the HUD then is white-cyan (0.95, 1, 1), (0.9, 1, 1), its dark shades and (0.16, 0.2, 0.2). This tells a marker's
	// colour by its hue (each channel over the brightest, within a_tol), any brightness.
	bool IsMgsvMarkerColor(float a_r, float a_g, float a_b, float a_tol = 0.06f);
}
