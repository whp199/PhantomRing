// Offline tests of Quiet's pure half (mgsv/QuietLogic.cpp; docs/research/quiet.md §3, §7): the save states and the borrow's plan by
// bits, the put-back's plan and the raw diff, her order record through two loads, her weapon, the commands taken over, the order job
// (Refused / Ordered / Hit / Released / Lost and its reasons), the stand-in rule, the grace sit, and the mirror's pull-in.
#include "QuietLogic.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>

using namespace phantom;
using namespace phantom::guest::quiet::plan;
using guest::logic::Vec3;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static void TestBits()
{
	// The player's save (live 2026-11-04, qstate.lua): gone.
	const auto gone = ParseSaveBits("obt=0 sortie=0 lost=1 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=1 bond=0 calls=46");
	CHECK(gone.known && !gone.obtained && gone.lost && gone.didLost && gone.bond == 0 && gone.calls == 46);
	CHECK(Classify(gone) == SaveState::kGone);
	CHECK(!ParseSaveBits("").known && !ParseSaveBits("error: attempt to index nil").known);
	CHECK(Classify(ParseSaveBits("garbage")) == SaveState::kUnknown);
	const auto present = ParseSaveBits("obt=1 sortie=1 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=100 calls=3");
	CHECK(Classify(present) == SaveState::kPresent);
	const auto recruited = ParseSaveBits("obt=1 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=20 calls=0");
	CHECK(Classify(recruited) == SaveState::kRecruited);
	const auto never = ParseSaveBits("obt=0 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=0 calls=0");
	CHECK(Classify(never) == SaveState::kNever);
	const auto killed = ParseSaveBits("obt=0 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=1 dis=0 gone=0 bond=0 calls=0");
	CHECK(Classify(killed) == SaveState::kKilled);
	// gvars.str_didLostQuiet alone counts as gone (LOST may have been cleared by a reunion and set again).
	CHECK(Classify(ParseSaveBits("obt=0 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=1 bond=0 calls=0")) == SaveState::kGone);
}

static bool Has(const std::vector<std::string>& a_v, const char* a_s)
{
	for (const auto& s : a_v) {
		if (s.find(a_s) != std::string::npos) {
			return true;
		}
	}
	return false;
}

static void TestPlan()
{
	const auto gone = ParseSaveBits("obt=0 sortie=0 lost=1 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=1 bond=0 calls=46");
	const auto present = ParseSaveBits("obt=1 sortie=1 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=100 calls=3");
	const auto recruited = ParseSaveBits("obt=1 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=20 calls=0");
	const auto never = ParseSaveBits("obt=0 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=0 dis=0 gone=0 bond=0 calls=0");
	const auto killed = ParseSaveBits("obt=0 sortie=0 lost=0 dead=0 hosp=0 dying=0 kill=1 dis=0 gone=0 bond=0 calls=0");
	// present: nothing; off: nothing.
	CHECK(BorrowPlan(present, BorrowMode::kAlways).empty());
	CHECK(BorrowPlan(gone, BorrowMode::kOff).empty());
	// recruited-not-deployable: sortie only.
	const auto r = BorrowPlan(recruited, BorrowMode::kMemory);
	CHECK(r.size() == 1 && Has(r, "SetSortieBuddyType"));
	// gone: the three calls of the live borrow (obtained, sortie, LOST unset).
	const auto g = BorrowPlan(gone, BorrowMode::kMemory);
	CHECK(g.size() == 3 && Has(g, "SetObtainedBuddyType") && Has(g, "SetSortieBuddyType") && Has(g, "UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)"));
	// never recruited / killed: nothing under memory, obtained + sortie under always; never BOSS_QUIET_KILL, the bond, a gvar or the reunion.
	CHECK(BorrowPlan(never, BorrowMode::kMemory).empty() && BorrowPlan(killed, BorrowMode::kMemory).empty());
	const auto n = BorrowPlan(never, BorrowMode::kAlways);
	CHECK(n.size() == 2 && Has(n, "SetObtainedBuddyType") && Has(n, "SetSortieBuddyType"));
	const auto k = BorrowPlan(killed, BorrowMode::kAlways);
	CHECK(k.size() == 2 && !Has(k, "BOSS_QUIET_KILL"));
	for (const auto* plan : { &g, &n, &k, &r }) {
		CHECK(!Has(*plan, "Reunion") && !Has(*plan, "gvars") && !Has(*plan, "FriendlyPoint"));
	}
	// sickbay / dying / dead: each unset only when its bit is set.
	auto sick = present;
	sick.hospitalize = true;
	sick.dead = true;
	const auto s = BorrowPlan(sick, BorrowMode::kMemory);
	CHECK(s.size() == 2 && Has(s, "BUDDY_QUIET_HOSPITALIZE") && Has(s, "UnsetDeadBuddyType") && !Has(s, "DYING") && !Has(s, "UnsetDisableBuddyType"));
	// The put-back's bits: back to the snapshot's.
	auto after = gone;
	after.obtained = after.sortie = true;
	after.lost = false;
	const auto back = RestorePlan(gone, after);
	CHECK(back.size() == 3 && Has(back, "UnsetObtainedBuddyType") && Has(back, "UnsetSortieBuddyType") && Has(back, "SetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)"));
	CHECK(RestorePlan(gone, gone).empty());
	// Borrowed on the player's save: still "gone" by str_didLostQuiet (a gvar is never touched), and nothing more to do.
	CHECK(Classify(after) == SaveState::kGone && BorrowPlan(after, BorrowMode::kAlways).empty());
	// A load that restored a bit wants exactly that bit again (the re-borrow).
	auto dropped = after;
	dropped.sortie = false;
	const auto again = BorrowPlan(dropped, BorrowMode::kAlways);
	CHECK(again.size() == 1 && Has(again, "SetSortieBuddyType"));
	dropped = gone;  // all three back
	CHECK(BorrowPlan(dropped, BorrowMode::kAlways).size() == 3);
}

static void TestDiff()
{
	std::array<std::uint8_t, kBlockBytes> snap{}, now{};
	for (std::size_t i = 0; i < kBlockBytes; ++i) {
		snap[i] = now[i] = static_cast<std::uint8_t>(i * 7);
	}
	CHECK(DiffBlock(snap.data(), now.data(), 0, 0).empty());
	// Another buddy's use: D-Dog's call count [2] (0x2D90) and its friendly point [1] (0x2DE6) -- explained, not Quiet's.
	now[0x2D90 - kBlockFirst] ^= 1;
	now[0x2DE6 - kBlockFirst] ^= 4;
	auto d = DiffBlock(snap.data(), now.data(), 0, 0);
	CHECK(d.size() == 2 && DiffClean(d));
	// D-Walker's obtained bit (bit 4) in the mask byte: not hers; her bit 3: hers.
	now[0x2D69 - kBlockFirst] ^= 0x10;
	CHECK(DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2D69 - kBlockFirst] ^= 0x08;
	CHECK(!DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2D69 - kBlockFirst] = snap[0x2D69 - kBlockFirst];
	// The common flag: bits 0-4 hers (LOST is bit 3), bit 5 (BUDDY_RIDE_HELI) not.
	now[0x2D80 - kBlockFirst] ^= 0x20;
	CHECK(DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2D80 - kBlockFirst] ^= 0x08;
	CHECK(!DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2D80 - kBlockFirst] = snap[0x2D80 - kBlockFirst];
	// Her call count [3] and her friendly point [0]: hers, whole bytes.
	now[0x2D95 - kBlockFirst] ^= 1;
	CHECK(!DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2D95 - kBlockFirst] = snap[0x2D95 - kBlockFirst];
	now[0x2DE4 - kBlockFirst] ^= 5;
	CHECK(!DiffClean(DiffBlock(snap.data(), now.data(), 0, 0)));
	now[0x2DE4 - kBlockFirst] = snap[0x2DE4 - kBlockFirst];
	// The static disable byte: her bit 3 only.
	CHECK(DiffClean(DiffBlock(snap.data(), now.data(), 0x00, 0x10)));
	CHECK(!DiffClean(DiffBlock(snap.data(), now.data(), 0x00, 0x08)));
	// The counters the put-back writes raw are all hers.
	for (const auto& [off, len] : CounterSpans()) {
		for (std::uint32_t i = 0; i < len; ++i) {
			CHECK(QuietMaskAt(off + i) == 0xFF);
		}
	}
	CHECK(QuietMaskAt(0x2D68) == 0 && QuietMaskAt(0x2E47) == 0 && QuietMaskAt(0x2E44) == 0xFF);
	// (live M1) Two sandbox loads with her borrowed left the mission-start copies borrowed: the put-back writes her bits back raw by
	// mask, other buddies' bits as they are now.
	CHECK(MergeMasked(0x1E, 0x16, kQuietBit) == 0x16);   // obtainedBuddyTypeMissionStart: her bit back off, D-Walker's (0x10) kept
	CHECK(MergeMasked(0x1E, 0x06, kQuietBit) == 0x16);   // another buddy's bit set meanwhile stays set
	CHECK(MergeMasked(0x20, 0x08, 0x1F) == 0x28);        // the common flag: LOST back, BUDDY_RIDE_HELI (bit 5) as now
	bool missionStart = false;
	for (const auto& [off, mask] : MaskedSpans()) {
		CHECK(mask == QuietMaskAt(off));
		missionStart |= off == 0x2D09 || off == 0x2D20;
	}
	CHECK(missionStart);
	// buddyFriendlyCount [0..7] are hers (live M1: [2] and [5] moved with her hits)
	CHECK(QuietMaskAt(0x2E06) == 0xFF && QuietMaskAt(0x2E0B) == 0xFF && QuietMaskAt(0x2E0C) == 0);
}

static void TestRecord()
{
	// [[sys+0xB0]+8] + i*0x200: two loads, never [sys+0xB0]+8+...
	std::map<std::uintptr_t, std::uint64_t> mem{ { 0x1000 + 0xB0, 0x5000 }, { 0x5000 + 8, 0x90000 } };
	const auto read = [&](std::uintptr_t a) -> std::optional<std::uint64_t> {
		const auto it = mem.find(a);
		return it == mem.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
	};
	CHECK(RecordAddress(read, 0x1000, 0) == 0x90000);
	CHECK(RecordAddress(read, 0x1000, 2) == 0x90400);
	CHECK(RecordAddress(read, 0x1000, 1) != 0x5000 + 8 + 0x200);
	CHECK(RecordAddress(read, 0, 0) == 0 && RecordAddress(read, 0x2000, 0) == 0);
	mem[0x5000 + 8] = 0;
	CHECK(RecordAddress(read, 0x1000, 0) == 0);
}

static void TestWeapon()
{
	CHECK(WeaponOfAttack(290) == proto::kQuietLethal && WeaponOfAttack(291) == proto::kQuietTranq && WeaponOfAttack(292) == proto::kQuietHeavy);
	CHECK(WeaponOfAttack(141) == proto::kQuietWeaponUnknown && WeaponOfAttack(0) == proto::kQuietWeaponUnknown);
	// Review (integration 0.9.0): the reported rifle is never "unknown" while her equipment names one; her dash (141 / 142) is no order's
	// round, whatever her equipment.
	CHECK(ShotWeapon(292, proto::kQuietTranq) == proto::kQuietHeavy && ShotWeapon(293, proto::kQuietTranq) == proto::kQuietTranq &&
	      ShotWeapon(0, proto::kQuietLethal) == proto::kQuietLethal && ShotWeapon(293, proto::kQuietWeaponUnknown) == proto::kQuietWeaponUnknown);
	CHECK(OrderRound(290, proto::kQuietWeaponUnknown) && OrderRound(293, proto::kQuietTranq) && !OrderRound(293, proto::kQuietWeaponUnknown) &&
	      !OrderRound(141, proto::kQuietHeavy) && !OrderRound(142, proto::kQuietLethal));
	CHECK(AttackOfWeapon(proto::kQuietHeavy) == 292 && AttackOfWeapon(proto::kQuietWeaponUnknown) == 0);
	CHECK(WeaponOfEquip(4) == proto::kQuietHeavy);  // the player's save: SINFUL BUTTERFLY, her hit 292
	CHECK(WeaponOfEquip(-1) == proto::kQuietWeaponUnknown && WeaponOfEquip(9) == proto::kQuietWeaponUnknown);
}

static void TestTakeOver()
{
	TakeOver off{};
	for (int c = 1; c <= 7; ++c) {
		CHECK(UseOf(c, off) == CommandUse::kPass);  // the layer off: MGSV's own, as 0.8.0
	}
	TakeOver t{ .layer = true, .orders = true, .cover = false, .talk = true };
	CHECK(UseOf(1, t) == CommandUse::kJob && UseOf(2, t) == CommandUse::kJob && UseOf(3, t) == CommandUse::kJob);
	CHECK(UseOf(4, t) == CommandUse::kRefuse);  // Cover me: not in Elden Ring yet
	CHECK(UseOf(5, t) == CommandUse::kPass && UseOf(6, t) == CommandUse::kPass);
	CHECK(UseOf(7, t) == CommandUse::kRefuse);  // the talk: MGSV's camera would turn to her body
	t.cover = true;
	t.talk = false;
	CHECK(UseOf(4, t) == CommandUse::kPass && UseOf(7, t) == CommandUse::kPass);
	t.orders = false;
	CHECK(UseOf(1, t) == CommandUse::kPass);
	CHECK(UseOf(8, t) == CommandUse::kPass && UseOf(0, t) == CommandUse::kPass);
}

static void TestJob()
{
	const JobSettings s{};
	// No pick: Refused after pickWaitMs (handle 0).
	{
		OrderJob j;
		j.Start(1, 1000);
		CHECK(!j.Update({ .nowMs = 1100 }, s).event);
		const auto st = j.Update({ .nowMs = 2600 }, s);
		CHECK(st.event == proto::kQuietRefused && !j.Busy() && j.Handle() == 0);
	}
	// Pick -> Realize -> Ordered -> Hit -> Released.
	{
		OrderJob j;
		j.Start(1, 1000);
		auto st = j.Update({ .nowMs = 1050, .pick = 0xAB }, s);
		CHECK(st.dedicate && j.Handle() == 0xAB && j.GetStage() == OrderJob::Stage::kRealize);
		st = j.Update({ .nowMs = 1200, .pick = 0xAB, .proxyReady = true }, s);
		CHECK(st.order && st.event == proto::kQuietOrdered && j.Engaged() && j.OrderedMs() == 1200);
		st = j.Update({ .nowMs = 4400, .pick = 0xAB, .proxyReady = true, .hit = true }, s);
		CHECK(st.event == proto::kQuietHit && !st.release && j.Engaged());
		st = j.Update({ .nowMs = 4430 }, s);
		CHECK(st.event == proto::kQuietReleased && st.release && !j.Busy());
	}
	// Realize timeout: Refused, released.
	{
		OrderJob j;
		j.Start(2, 0);
		j.Update({ .nowMs = 10, .pick = 5 }, s);
		const auto st = j.Update({ .nowMs = 1600, .pick = 5 }, s);
		CHECK(st.event == proto::kQuietRefused && st.release && !j.Busy());
	}
	// No hit in 8 s: Lost (MGSV).
	{
		OrderJob j;
		j.Start(1, 0);
		j.Update({ .nowMs = 10, .pick = 5 }, s);
		j.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		CHECK(!j.Update({ .nowMs = 8000, .pick = 5 }, s).event);
		const auto st = j.Update({ .nowMs = 8021, .pick = 5 }, s);
		CHECK(st.event == proto::kQuietLost && st.lost == proto::kQuietLostMgsv && st.release);
	}
	// Take aim: held 20 s, then Lost; a hit within it: Hit.
	{
		OrderJob j;
		j.Start(3, 0);
		j.Update({ .nowMs = 10, .pick = 5 }, s);
		j.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		CHECK(!j.Update({ .nowMs = 15000, .pick = 5 }, s).event);
		CHECK(j.Update({ .nowMs = 16000, .pick = 5, .hit = true }, s).event == proto::kQuietHit);
		OrderJob k;
		k.Start(3, 0);
		k.Update({ .nowMs = 10, .pick = 5 }, s);
		k.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		const auto st = k.Update({ .nowMs = 20021, .pick = 5 }, s);
		CHECK(st.event == proto::kQuietLost && st.lost == proto::kQuietLostMgsv);
	}
	// The target dies: Lost (target); a rest / load / dismissal: Lost (cancelled), released at once.
	{
		OrderJob j;
		j.Start(1, 0);
		j.Update({ .nowMs = 10, .pick = 5 }, s);
		j.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		const auto st = j.Update({ .nowMs = 900, .targetPresent = false }, s);
		CHECK(st.event == proto::kQuietLost && st.lost == proto::kQuietLostTarget && st.release);
		OrderJob k;
		k.Start(1, 0);
		k.Update({ .nowMs = 10, .pick = 5 }, s);
		k.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		const auto c = k.Update({ .nowMs = 900, .cancel = true }, s);
		CHECK(c.event == proto::kQuietLost && c.lost == proto::kQuietLostCancelled && c.release && !k.Busy());
		// her "can't": Lost (MGSV) at once
		OrderJob m;
		m.Start(1, 0);
		m.Update({ .nowMs = 10, .pick = 5 }, s);
		m.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		CHECK(m.Update({ .nowMs = 2000, .pick = 5, .cant = true }, s).lost == proto::kQuietLostMgsv);
	}
	// Abort: by stage.
	{
		OrderJob j;
		j.Start(1, 0);
		CHECK(j.Abort("x").event == proto::kQuietRefused);
		j.Start(1, 0);
		j.Update({ .nowMs = 10, .pick = 5 }, s);
		j.Update({ .nowMs = 20, .pick = 5, .proxyReady = true }, s);
		const auto a = j.Abort("a buddy change");
		CHECK(a.event == proto::kQuietLost && a.lost == proto::kQuietLostCancelled && a.release && !j.Busy());
		CHECK(!j.Abort("idle").event);
	}
	// The stand-in outside a job: while the pick holds and the binoculars are or were up within keepMs.
	CHECK(!WantStandIn(0, true, 0, 100, 20000));
	CHECK(WantStandIn(7, true, 0, 100, 20000));
	CHECK(WantStandIn(7, false, 1000, 20999, 20000) && !WantStandIn(7, false, 1000, 21001, 20000));
	CHECK(!WantStandIn(7, false, 0, 100, 20000));
}

static void TestSit()
{
	const Vec3 nan{ NAN, NAN, NAN };
	// The v1b watch faces out: from the grace through the seat.
	const Vec3 centre{ 0.0f, 0.0f, 0.0f }, seat{ 2.0f, 0.0f, 0.0f };
	const float toGrace = std::atan2(-2.0f, 0.0f);
	CHECK(Near(SitYaw(centre, seat, toGrace, SitPose::kKneel), toGrace));
	CHECK(Near(SitYaw(centre, seat, toGrace, SitPose::kWatch), std::atan2(2.0f, 0.0f)));
	CHECK(std::fabs(std::remainder(SitYaw(nan, seat, toGrace, SitPose::kWatch) - toGrace - std::numbers::pi_v<float>, 2.0f * std::numbers::pi_v<float>)) < 1e-3f);

	SitHold h;
	SitInput in{ .out = true, .canRest = true, .atGrace = true, .graceSeq = 1, .centre = centre, .seat = seat, .seatYaw = toGrace, .her = Vec3{ 10, 0, 10 }, .placeable = true };
	// Far from the seat: put 2.5 m off it first (live 0.9.0: her order from 47 m played a long arrival leap after the black), re-placed
	// while that hasn't landed (never ordered from afar); from there her own order (aim off: the rifle down on arrival), then placed
	// at the seat once the Lua thread sent it.
	auto st = h.Update(in, SitPose::kKneel);
	CHECK(st.place && Near(st.place->x, 4.5f) && !st.moveTo && h.Seated() == 0);
	st = h.Update(in, SitPose::kKneel);
	CHECK(st.place && !st.moveTo);  // still 14 m off: placed again, not ordered
	in.her = Vec3{ 4.5f, 0, 0 };
	st = h.Update(in, SitPose::kKneel);
	CHECK(st.moveTo && Near(st.moveTo->x, 2.0f) && !st.moveAim && st.moveId == 1 && !st.place && h.Seated() == 0);
	st = h.Update(in, SitPose::kKneel);
	CHECK(!st.place && !st.moveTo);  // not sent yet: nothing
	in.moveSent = 1;
	in.nowMs = 10000;
	st = h.Update(in, SitPose::kKneel);
	CHECK(st.place && Near(st.place->x, 2.0f) && h.Seated() == 0);
	in.her = seat;
	in.nowMs = 10500;
	st = h.Update(in, SitPose::kKneel);
	CHECK(!st.place && h.Seated() == 0);  // at the seat, but not settled yet (MGSV's Quiet turns, hops and crouches for ~3 s: live 0.9.0)
	in.her = Vec3{ 2.6f, 0, 0 };  // her hop: 0.6 m off
	in.nowMs = 12900;
	st = h.Update(in, SitPose::kKneel);
	CHECK(st.place && h.Seated() == 0);  // placed again, still not echoed
	in.her = seat;
	in.nowMs = 13000;
	CHECK(h.Update(in, SitPose::kKneel).place == std::nullopt && h.Seated() == 0);
	in.nowMs = 13250;  // 3.25 s after her order, but still for 250 ms only
	CHECK(h.Update(in, SitPose::kKneel).place == std::nullopt && h.Seated() == 0);
	in.nowMs = 13350;  // still for 350 ms: settled -- the echo
	st = h.Update(in, SitPose::kKneel);
	CHECK(!st.place && h.Seated() == 1);
	st = h.Update(in, SitPose::kKneel);
	CHECK(!st.place && !st.moveTo && h.Seated() == 1);
	// A drift past 0.3 m: re-placed (one-frame method only).
	in.her = Vec3{ 2.5f, 0.0f, 0.0f };
	CHECK(h.Update(in, SitPose::kKneel).place);
	in.placeable = false;
	CHECK(!h.Update(in, SitPose::kKneel).place);
	in.placeable = true;
	// Snake leaves the grace: up (her order with aim).
	in.atGrace = false;
	st = h.Update(in, SitPose::kKneel);
	CHECK(st.getUp && h.Seated() == 0 && !h.Busy());
	CHECK(!h.Update(in, SitPose::kKneel).getUp);  // once
	// Already within 2 m of the seat: 2.5 m off first (MoveToPosition ignores a nearer point), then the order, then the seat.
	SitHold n;
	SitInput nin{ .out = true, .canRest = true, .atGrace = true, .graceSeq = 5, .centre = centre, .seat = seat, .seatYaw = toGrace, .her = Vec3{ 2.5f, 0, 0 }, .placeable = true };
	st = n.Update(nin, SitPose::kKneel);
	CHECK(st.place && Near(st.place->x, 4.5f) && !st.moveTo);  // away from the grace, through the seat
	nin.her = *st.place;
	st = n.Update(nin, SitPose::kKneel);
	CHECK(st.moveTo && Near(st.moveTo->x, 2.0f));
	nin.moveSent = st.moveId;
	nin.nowMs = 1000;
	st = n.Update(nin, SitPose::kKneel);
	CHECK(st.place && Near(st.place->x, 2.0f));
	// She never stands still (an endless fidget): seated at kSitSettleMaxMs anyway.
	for (std::uint64_t t = 1100; t <= 5600; t += 100) {
		nin.her = Vec3{ 2.0f + (t % 200 ? 0.05f : 0.0f), 0, 0 };
		nin.nowMs = t;
		n.Update(nin, SitPose::kKneel);
	}
	CHECK(n.Seated() == 5);
	// v1b's watch keeps her aim on (the order with aim).
	SitHold wv;
	SitInput win = in;
	win.atGrace = true;
	win.graceSeq = 7;
	win.her = Vec3{ 9, 0, 9 };
	win.moveSent = 0;
	st = wv.Update(win, SitPose::kWatch);
	CHECK(st.place && !st.moveTo);
	win.her = *st.place;
	st = wv.Update(win, SitPose::kWatch);
	CHECK(st.moveTo && st.moveAim);
	// A rest with no seat (a talk: NaN): nothing at all.
	SitHold t;
	SitInput talk{ .out = true, .canRest = true, .atGrace = true, .graceSeq = 2, .seat = nan, .her = Vec3{}, .placeable = true };
	st = t.Update(talk, SitPose::kKneel);
	CHECK(!st.place && !st.moveTo && !t.Busy());
	// No placement method: nothing (the host's deadline keeps her hidden).
	SitHold u;
	in = { .out = true, .canRest = true, .atGrace = true, .graceSeq = 3, .centre = centre, .seat = seat, .seatYaw = 0, .her = Vec3{ 9, 0, 9 }, .placeable = false };
	st = u.Update(in, SitPose::kKneel);
	CHECK(!st.place && !st.moveTo && u.Seated() == 0);
	// RestAtGrace off: nothing.
	SitHold v;
	in.placeable = true;
	in.canRest = false;
	CHECK(!v.Update(in, SitPose::kKneel).moveTo);
	// A re-rest (graceSeq bumped): her order again.
	SitHold w;
	in.canRest = true;
	in.her = Vec3{ 4.5f, 0, 0 };
	st = w.Update(in, SitPose::kKneel);
	in.moveSent = st.moveId;
	in.nowMs = 20000;
	w.Update(in, SitPose::kKneel);
	in.her = seat;
	in.nowMs = 20100;
	w.Update(in, SitPose::kKneel);
	in.nowMs = 24600;
	w.Update(in, SitPose::kKneel);
	CHECK(w.Seated() == 3);
	in.graceSeq = 4;
	in.her = Vec3{ 4.5f, 0, 0 };
	st = w.Update(in, SitPose::kKneel);
	CHECK(st.moveTo && st.moveId == 2 && w.Seated() == 0);
	// The placement 2.5 m off never lands (6 tries): her order from where she is (the old way) rather than no seat.
	SitHold y;
	SitInput yin{ .out = true, .canRest = true, .atGrace = true, .graceSeq = 9, .centre = centre, .seat = seat, .seatYaw = toGrace, .her = Vec3{ 20, 0, 0 }, .placeable = true };
	int places = 0;
	st = {};
	for (int i = 0; i < 8 && !st.moveTo; ++i) {
		st = y.Update(yin, SitPose::kKneel);
		places += st.place ? 1 : 0;
	}
	CHECK(st.moveTo && places == 7);
}

static void TestMirror()
{
	const Vec3 c{ 0, -12, 0 }, snake{ 0, -12, 0 }, cam{ 0, -10.5f, -5 };
	const std::vector<float> one{ 15.0f };
	// Inside: unchanged (y = the ground's).
	auto p = MirrorPoint({ 5, 3, 8 }, snake, cam, c, one, -12.0f);
	CHECK(Near(p.x, 5) && Near(p.z, 8) && Near(p.y, -12));
	// Outside: pulled in along the camera's line onto the region's edge.
	p = MirrorPoint({ 0, 0, 40 }, snake, cam, c, one, -12.0f);
	CHECK(Near(p.x, 0, 0.01f) && Near(p.z, 15, 0.01f));
	p = MirrorPoint({ 30, 0, 30 }, snake, cam, c, one, -12.0f);
	CHECK(InRegion(p, c, one) && Near((p - c).LengthXZ(), 15, 0.05f));
	// The direction from the camera is kept (MGSV's frustum still holds her).
	const float a0 = std::atan2(30.0f - cam.x, 30.0f - cam.z), a1 = std::atan2(p.x - cam.x, p.z - cam.z);
	CHECK(Near(a0, a1, 1e-3f));
	// Never nearer than 3 m to Snake.
	p = MirrorPoint({ 0.5f, 0, 1 }, snake, cam, c, one, -12.0f);
	CHECK((p - snake).LengthXZ() >= 2.99f);
	// By bearing: 8 radii, a short one north (+z), long elsewhere.
	const std::vector<float> eight{ 10, 40, 40, 40, 40, 40, 40, 40 };
	CHECK(Near(RegionRadius(eight, 0.0f), 10) && Near(RegionRadius(eight, std::numbers::pi_v<float> / 4), 40));
	CHECK(Near(RegionRadius(eight, std::numbers::pi_v<float> / 8), 25));
	p = MirrorPoint({ 0, 0, 30 }, snake, { 0, 0, -1 }, c, eight, -12.0f);
	CHECK(Near(p.z, 10, 0.02f));
	p = MirrorPoint({ 30, 0, 0 }, snake, { 0, 0, -1 }, c, eight, -12.0f);
	CHECK(Near(p.x, 30, 0.02f));
	// A region bearing wraps at 2 pi.
	CHECK(Near(RegionRadius(eight, 2.0f * std::numbers::pi_v<float> - 0.001f), RegionRadius(eight, -0.001f), 1e-2f));
}

int main()
{
	TestBits();
	TestPlan();
	TestDiff();
	TestRecord();
	TestWeapon();
	TestTakeOver();
	TestJob();
	TestSit();
	TestMirror();
	if (g_failures) {
		std::printf("test_guest_quiet: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_guest_quiet: all passed\n");
	return 0;
}
