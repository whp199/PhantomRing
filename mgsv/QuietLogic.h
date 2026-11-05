#pragma once
// Quiet as a sniper buddy in Elden Ring, the pure half (docs/research/quiet.md §3; the game glue in Quiet.cpp, tests in
// tests/test_guest_quiet.cpp). No game, Windows or mapping access here.
//
//  - The save's Quiet, told apart by bits (§2.2): present / recruited-not-deployable / gone / never recruited / killed, and the
//    memory-only borrow's plan for each (§3.1); the raw snapshot's diff that proves the put-back left none of Quiet's bytes changed.
//  - Her order record R = [[sys+0xB0]+8] + index*0x200 (two loads, §2.4).
//  - Her weapon from MGSV's attack id at the hit (290 / 291 / 292) and from vars.buddyQuietEquipType (Q11).
//  - The order job (§3.5): the player's Fire / Take aim carried out on the Elden Ring enemy the host picks (kActorQuietTarget): one
//    inert MGSV stand-in (ActorProxy Dedicate), MGSV's own command on him, her hit on him seen at its sender = kGEvQuietShot Hit.
//  - The grace sit (§3.9): a new graceSeq with a seat -> placed, then her rifle stood down; the flag falling -> she gets up.
//  - The mirror (§3.2): her MGSV body at the post's MGSV point, pulled in along MGSV's camera line into the apron's free region.

#include "Logic.h"
#include "phantomring_protocol.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::guest::quiet::plan
{
	using logic::Vec3;

	// ---- the save's Quiet (§2.2) -------------------------------------------------------------------------------------------------
	struct SaveBits
	{
		bool known{ false };
		bool obtained{ false };     // TppBuddyService.DidObtainBuddyType(QUIET)
		bool sortie{ false };       // CanSortieBuddyType(QUIET)
		bool lost{ false };         // BuddyCommonFlag.BUDDY_QUIET_LOST
		bool dead{ false };         // IsDeadBuddyType(QUIET)
		bool hospitalize{ false };  // BUDDY_QUIET_HOSPITALIZE
		bool dying{ false };        // BUDDY_QUIET_DYING
		bool killed{ false };       // BOSS_QUIET_KILL
		bool disabled{ false };     // IsDisableBuddyType(QUIET)
		bool didLost{ false };      // gvars.str_didLostQuiet
		int  bond{ -1 };            // GetFriendlyPoint(BuddyFriendlyType.QUIET)
		int  calls{ -1 };           // vars.buddyCallCount[QUIET]
	};
	// PR.QuietState()'s answer: "obt=1 sortie=0 lost=1 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=1 bond=0 calls=46" (any order; missing
	// words keep their default; nothing parsed = unknown).
	SaveBits    ParseSaveBits(std::string_view a_text);
	std::string DescribeBits(const SaveBits& a_bits);

	enum class SaveState : std::uint8_t
	{
		kUnknown,
		kPresent,    // obtained, sortie, not LOST, not dead / in sickbay: she deploys as she is
		kRecruited,  // obtained, not sortie-able yet (side op mtbs_q99011), not LOST
		kGone,       // "A Quiet Exit": LOST or str_didLostQuiet (the player's save)
		kNever,      // never recruited: not obtained, not LOST
		kKilled,     // never recruited because she was killed in mission 11 (BOSS_QUIET_KILL)
	};
	SaveState   Classify(const SaveBits& a_bits);
	const char* StateName(SaveState a_state);
	// The sub-states of present / recruited that keep her from deploying (each borrowed by its own unset).
	bool Sick(const SaveBits& a_bits);

	enum class BorrowMode : std::uint8_t
	{
		kOff,
		kMemory,  // a Quiet the save has had: gone, recruited-not-deployable, sickbay / dying / dead
		kAlways,  // also never recruited and killed
	};
	// Whether this state is borrowed under a_mode (present with no sick bit: nothing to borrow).
	bool BorrowWanted(SaveState a_state, const SaveBits& a_bits, BorrowMode a_mode);
	// The Lua statements of the plan (§3.1), in order: TppBuddyService calls only, never a gvar, the bond, RequestReunionQuiet or
	// BOSS_QUIET_KILL. Empty = nothing to do (present, unknown, or not wanted under a_mode).
	std::vector<std::string> BorrowPlan(const SaveBits& a_bits, BorrowMode a_mode);
	// The put-back's Lua statements that set the bits back to a_snap's (the counters go back by the raw bytes).
	std::vector<std::string> RestorePlan(const SaveBits& a_snap, const SaveBits& a_now);

	// ---- the raw buddy vars block and the static disable byte (§2.2, §3.1) ------------------------------------------------------------
	inline constexpr std::uint32_t kBlockFirst = 0x2D08, kBlockEnd = 0x2E48;  // [vars] + [first, end): every buddy var
	inline constexpr std::size_t   kBlockBytes = kBlockEnd - kBlockFirst;
	inline constexpr std::uint8_t  kQuietBit = 1u << 3;                          // BuddyType.QUIET's bit in the type masks
	struct ByteDiff
	{
		std::uint32_t offset{ 0 };  // into vars
		std::uint8_t  before{ 0 }, after{ 0 };
		bool          quiet{ false };  // a bit of Quiet's own fields differs
		const char*   field{ "" };
	};
	// Quiet's bits of one byte of the block (0 = none of hers): the type masks' bit 3, the common flags' bits 0-4, her counters'
	// whole bytes (call count [3], friendly timer / point / max point [0] and their mission-start copy, equip, costume).
	std::uint8_t QuietMaskAt(std::uint32_t a_offset);
	const char*  FieldAt(std::uint32_t a_offset);
	// Every byte of a_now that differs from a_snap (both kBlockBytes long), with whether a bit of Quiet's differs; and the static byte
	// (0x142C2919E: SetDisableBuddyType's, outside vars; Quiet's bit 3) as offset 0xFFFFFFFF.
	std::vector<ByteDiff> DiffBlock(const std::uint8_t* a_snap, const std::uint8_t* a_now, std::uint8_t a_staticSnap, std::uint8_t a_staticNow);
	// The pass rule: no Quiet byte differs.
	bool        DiffClean(const std::vector<ByteDiff>& a_diff);
	std::string DescribeDiff(const std::vector<ByteDiff>& a_diff);
	// The offsets of Quiet's counters that the put-back writes back raw (whole bytes): a_out gets (offset, length) pairs.
	std::vector<std::pair<std::uint32_t, std::uint32_t>> CounterSpans();
	// Quiet's bits in the type masks and common flags that the put-back restores raw by mask after the Lua setters: above all the
	// `*MissionStart` copies, which MGSV fills from vars at every mission start (live M1: two sandbox loads with her borrowed left
	// obtained / sortie / LOST borrowed in 0x2D09 / 0x2D0A / 0x2D20 after the setters had put the live bits back). (offset, mask)
	std::vector<std::pair<std::uint32_t, std::uint8_t>> MaskedSpans();
	// The byte to write back: a_now's other bits, a_snap's Quiet bits.
	inline std::uint8_t MergeMasked(std::uint8_t a_now, std::uint8_t a_snap, std::uint8_t a_mask) { return static_cast<std::uint8_t>((a_now & ~a_mask) | (a_snap & a_mask)); }

	// ---- her order record (§2.4) -------------------------------------------------------------------------------------------------
	inline constexpr std::uint32_t kRecTargetActive = 0x180, kRecTargetRequested = 0x182, kRecTargetThird = 0x184, kRecTargetAim = 0x186,
	                               kRecDisabled = 0x198, kRecActive = 0x19C, kRecRequested = 0x1A0, kRecCancel = 0x1A4, kRecDelay = 0x1AC;
	inline constexpr std::uint32_t kDisabledFire = 1u << 4, kDisabledAim = 1u << 5;  // R+0x198 (set = off)
	// R = [[sys+0xB0]+8] + index*0x200; a_read(address) -> the u64 there (nullopt = unreadable). 0 when a load fails.
	template <class Read>
	std::uintptr_t RecordAddress(Read&& a_read, std::uintptr_t a_sys, std::uint32_t a_index)
	{
		if (!a_sys) {
			return 0;
		}
		const std::optional<std::uint64_t> base = a_read(a_sys + 0xB0);
		if (!base || !*base) {
			return 0;
		}
		const std::optional<std::uint64_t> arr = a_read(static_cast<std::uintptr_t>(*base) + 8);
		if (!arr || !*arr) {
			return 0;
		}
		return static_cast<std::uintptr_t>(*arr) + static_cast<std::uintptr_t>(a_index) * 0x200;
	}

	// ---- her weapon (Q11) ---------------------------------------------------------------------------------------------------------
	inline constexpr std::uint32_t kAtkWicked = 290, kAtkGuilty = 291, kAtkSinful = 292;
	proto::QuietWeapon WeaponOfAttack(std::uint32_t a_attackId);  // 290 lethal, 291 tranq, 292 heavy, else unknown
	std::uint32_t      AttackOfWeapon(proto::QuietWeapon a_weapon);
	// vars.buddyQuietEquipType -> the rifle (Q11 decides the table; until then: 0-1 Wicked, 2-3 Guilty, 4-5 Sinful, else unknown).
	proto::QuietWeapon WeaponOfEquip(int a_equip);
	const char*        WeaponName(proto::QuietWeapon a_weapon);
	// The rifle a hit is reported with: the attack id's, else her equipment var's (an id outside 290-292: a suppressed variant's,
	// unverified) -- never "unknown" while either is known (review: unknown fell through to lethal on the host).
	proto::QuietWeapon ShotWeapon(std::uint32_t a_attackId, proto::QuietWeapon a_equip);
	// Her hit on the order's stand-in counts as the order's round: her rifle's (not her dash attacks 141 / 142, which send the same
	// message) and a rifle known by the id or her equipment (review).
	bool               OrderRound(std::uint32_t a_attackId, proto::QuietWeapon a_equip);
	const char*        PhaseName(proto::QuietPhase a_phase);

	// ---- the player's commands (§3.3) ----------------------------------------------------------------------------------------------
	inline constexpr int kCmdFireKill = 1, kCmdFireTranq = 2, kCmdAim = 3, kCmdCover = 4, kCmdCease = 5, kCmdShootThis = 6, kCmdTalk = 7;
	inline constexpr bool IsQuietCommand(int a_cmd) { return a_cmd >= kCmdFireKill && a_cmd <= kCmdTalk; }
	inline constexpr bool IsOrderCommand(int a_cmd) { return a_cmd >= kCmdFireKill && a_cmd <= kCmdAim; }
	struct TakeOver
	{
		bool layer{ false };     // kHostQuietLayer and [Quiet] Enabled and her sites
		bool orders{ false };    // [Quiet] Orders
		bool cover{ false };     // [Quiet] Cover
		bool talk{ false };      // [Quiet] TakeOverTalk
	};
	// What the guest does with the player's command a_cmd while Quiet is out: kPass = MGSV's own (as 0.8.0), kJob = the order job on the
	// host's pick, kRefuse = not passed to MGSV, nothing done ("not in Elden Ring yet" / the talk's "Quiet").
	enum class CommandUse : std::uint8_t { kPass, kJob, kRefuse };
	CommandUse UseOf(int a_cmd, const TakeOver& a_t);

	// ---- the order job (§3.5) ----------------------------------------------------------------------------------------------------
	struct JobSettings
	{
		std::uint64_t pickWaitMs{ 1500 };        // the order waits this long for a pick (the host may be re-posting her)
		std::uint64_t realizeTimeoutMs{ 1500 };  // the stand-in realized and placed within this, else Refused
		std::uint64_t hitTimeoutMs{ 8000 };      // Fire: order -> hit (live 3.1-4.3 s), else Lost (MGSV)
		std::uint64_t aimHoldMs{ 20000 };        // Take aim: held until her hit
	};
	struct JobInput
	{
		std::uint64_t nowMs{ 0 };
		std::uint64_t pick{ 0 };          // the host's kActorQuietTarget (hostile, alive) now, else the one seen within 1 s before
		bool          targetPresent{ true };  // the job's target is still in the table, hostile and alive
		bool          proxyReady{ false };    // its stand-in is realized and placed (ActorProxy Dedicated.ready)
		bool          hit{ false };           // her hit on the job's soldier since the order
		bool          cant{ false };          // her "can't" path (kQuietNoTarget) ran for the order
		bool          cancel{ false };        // a release trigger (a rest, a load, Tarnished mode, the layer off, a dismissal, a buddy change)
	};
	struct JobStep
	{
		std::optional<proto::QuietPhase> event;
		proto::QuietLostReason           lost{ proto::kQuietLostMgsv };  // with event == kQuietLost
		bool                             dedicate{ false };  // a stand-in for handle() now
		bool                             order{ false };     // send MGSV's command (cmd()) on the stand-in now
		bool                             release{ false };   // the stand-in's order is over
		const char*                      why{ "" };
	};
	class OrderJob
	{
	public:
		enum class Stage : std::uint8_t { kIdle, kPick, kRealize, kOrdered, kHit };
		JobStep Start(int a_cmd, std::uint64_t a_nowMs);
		JobStep Update(const JobInput& a_in, const JobSettings& a_s);
		// Ends a running job (a release trigger seen outside Update): Lost/cancelled when ordered, else nothing to report.
		JobStep Abort(const char* a_why);
		[[nodiscard]] bool          Busy() const { return m_stage != Stage::kIdle; }
		[[nodiscard]] bool          Engaged() const { return m_stage == Stage::kOrdered || m_stage == Stage::kHit; }
		[[nodiscard]] Stage         GetStage() const { return m_stage; }
		[[nodiscard]] std::uint64_t Handle() const { return m_handle; }
		[[nodiscard]] int           Cmd() const { return m_cmd; }
		[[nodiscard]] bool          Aim() const { return m_cmd == kCmdAim; }
		[[nodiscard]] std::uint64_t OrderedMs() const { return m_orderMs; }

	private:
		Stage         m_stage{ Stage::kIdle };
		int           m_cmd{ 0 };
		std::uint64_t m_handle{ 0 };
		std::uint64_t m_startMs{ 0 }, m_realizeMs{ 0 }, m_orderMs{ 0 };
	};
	const char* StageName(OrderJob::Stage a_stage);

	// The stand-in outside a job (MarkProxy, §3.4): while the host's pick exists and the binoculars are up, or were up within keepMs.
	bool WantStandIn(std::uint64_t a_pick, bool a_binoculars, std::uint64_t a_lastBinocularsMs, std::uint64_t a_nowMs, std::uint64_t a_keepMs);

	// ---- the grace sit (§3.9) ----------------------------------------------------------------------------------------------------
	enum class SitPose : std::uint8_t { kKneel, kWatch, kSeat };
	inline constexpr float kSitOrderFar = 3.5f;  // m: her order to the seat goes out from at most this far (else 2.5 m off first)
	// (0.9.0 integration, live) After her order MGSV's Quiet takes ~3 s to settle into her rest at the seat -- she turns, hops (up to
	// 0.7 m) and crouches with her rifle upright -- longer than the rest's black (~1.5 s): the seat is echoed (kBuddySeated) only once
	// she has settled, kSitSettleMs after the order and still for kSitStillMs (at the latest kSitSettleMaxMs); the host fades her in.
	inline constexpr std::uint64_t kSitSettleMs = 3200, kSitStillMs = 300, kSitSettleMaxMs = 4500;
	struct SitInput
	{
		bool          out{ false };       // Quiet is out (and the layer on)
		bool          canRest{ false };   // [Quiet] RestAtGrace
		bool          atGrace{ false };   // HostState kHostAtGrace
		std::uint32_t graceSeq{ 0 };
		Vec3          centre;             // MGSV world, treadmill-corrected (NaN = unknown)
		Vec3          seat;               // NaN = no seat
		float         seatYaw{ 0.0f };    // MGSV yaw from the seat toward the grace
		std::optional<Vec3> her;          // where she stands now (her location object's feet / GetPosition)
		bool          placeable{ false }; // a one-frame placement is available now
		std::uint32_t moveSent{ 0 };      // the last SitStep::moveId the Lua thread has sent (MoveToPosition)
		std::uint64_t nowMs{ 0 };         // (settling: she is echoed seated once she has stood still after her order)
	};
	struct SitStep
	{
		std::optional<Vec3> place;  // put her there (feet) facing placeYaw, by the one-frame method
		float               placeYaw{ 0.0f };
		// (live M1, quiet.md §2.10 Q2) her own order to the seat with her aim off: she rests there with the rifle held upright once she
		// arrives -- and she arrives at once, placed there right after it (~0.4 s to the rifle down). MoveToPosition ignores a point
		// within 2 m of her, so she is put 2.5 m off first when she is nearer.
		std::optional<Vec3> moveTo;
		bool                moveAim{ false };  // disableAim = !moveAim (kKneel / kSeat: false -> the rifle down; kWatch: true)
		std::uint32_t       moveId{ 0 };       // the order's id: the Lua thread reports it sent (SitInput::moveSent)
		bool                getUp{ false };    // her aim on again and a move order with aim (Snake left the grace)
		const char*         why{ "" };
	};
	class SitHold
	{
	public:
		SitStep Update(const SitInput& a_in, SitPose a_pose);
		// The rest she holds the seat of (0 = none): kBuddySeated + GuestState.graceSeqSeated while non-zero.
		[[nodiscard]] std::uint32_t Seated() const { return m_seated; }
		[[nodiscard]] bool          Busy() const { return m_want != 0; }
		[[nodiscard]] Vec3          Seat() const { return m_seat; }
		void                        Reset() { *this = {}; }

	private:
		enum class Stage : std::uint8_t { kNone, kStart, kAway, kMove, kPlace, kHeld };
		Stage         m_stage{ Stage::kNone };
		std::uint32_t m_want{ 0 };     // the rest being seated
		std::uint32_t m_seated{ 0 };   // the rest seated (placed after her order)
		std::uint32_t m_lastSeq{ 0 };  // the last graceSeq taken (a seq is placed once; a re-rest bumps it)
		std::uint32_t m_moveId{ 0 };
		Vec3          m_seat, m_away;
		float         m_yaw{ 0.0f };
		int           m_tries{ 0 };
		bool          m_ordered{ false };  // her order to the seat went out this rest (getting up needs a new one)
		std::uint64_t m_orderMs{ 0 }, m_stillSince{ 0 };
		Vec3          m_last;
		bool          m_haveLast{ false };
	};
	// The yaw she faces at the seat: kKneel / kSeat toward the grace (the host's seatYaw); kWatch facing out, from the grace through
	// the seat (her raised rifle points away from Snake and the light). A NaN centre keeps the host's yaw turned round.
	float SitYaw(const Vec3& a_centre, const Vec3& a_seat, float a_seatYaw, SitPose a_pose);

	// ---- the mirror (§3.2) -------------------------------------------------------------------------------------------------------
	// The apron's free region round the sandbox centre (Q7): one radius, or N radii by bearing (N >= 8, bearing 0 = +z, clockwise to
	// +x as atan2(dx, dz)); linear between neighbours.
	float RegionRadius(const std::vector<float>& a_radii, float a_bearing);
	bool  InRegion(const Vec3& a_point, const Vec3& a_centre, const std::vector<float>& a_radii);
	// Her MGSV place for a post mapped to a_want: a_want when inside the region; else pulled in along MGSV's camera line (a_cam ->
	// a_want) to the farthest point inside it; never nearer than a_minSnake to Snake (pushed out along the same line). y = a_groundY.
	Vec3 MirrorPoint(const Vec3& a_want, const Vec3& a_snake, const Vec3& a_cam, const Vec3& a_centre, const std::vector<float>& a_radii, float a_groundY,
		float a_minSnake = 3.0f);
}
