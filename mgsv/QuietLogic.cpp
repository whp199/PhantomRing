// Quiet as a sniper buddy in Elden Ring, the pure half (QuietLogic.h; docs/research/quiet.md §3).
#include "QuietLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <numbers>

namespace phantom::guest::quiet::plan
{
	// ---- the save's Quiet -------------------------------------------------------------------------------------------------------
	SaveBits ParseSaveBits(std::string_view a_text)
	{
		SaveBits b;
		int      words = 0;
		std::size_t at = 0;
		while (at < a_text.size()) {
			const auto end = std::min(a_text.find(' ', at), a_text.size());
			const auto word = a_text.substr(at, end - at);
			at = end + 1;
			const auto eq = word.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto key = word.substr(0, eq);
			const auto valueText = std::string(word.substr(eq + 1));
			int        value = 0;
			if (std::sscanf(valueText.c_str(), "%d", &value) != 1) {
				continue;
			}
			const bool on = value != 0;
			++words;
			if (key == "obt") {
				b.obtained = on;
			} else if (key == "sortie") {
				b.sortie = on;
			} else if (key == "lost") {
				b.lost = on;
			} else if (key == "dead") {
				b.dead = on;
			} else if (key == "hosp") {
				b.hospitalize = on;
			} else if (key == "dying") {
				b.dying = on;
			} else if (key == "kill") {
				b.killed = on;
			} else if (key == "dis") {
				b.disabled = on;
			} else if (key == "gone") {
				b.didLost = on;
			} else if (key == "bond") {
				b.bond = value;
			} else if (key == "calls") {
				b.calls = value;
			} else {
				--words;
			}
		}
		b.known = words >= 4;
		return b;
	}

	std::string DescribeBits(const SaveBits& a_b)
	{
		if (!a_b.known) {
			return "unknown";
		}
		std::string s = std::format("obtained {} sortie {}", a_b.obtained, a_b.sortie);
		for (const auto& [on, name] : { std::pair{ a_b.lost, "LOST" }, std::pair{ a_b.didLost, "str_didLostQuiet" }, std::pair{ a_b.dead, "dead" },
				 std::pair{ a_b.hospitalize, "HOSPITALIZE" }, std::pair{ a_b.dying, "DYING" }, std::pair{ a_b.killed, "BOSS_QUIET_KILL" },
				 std::pair{ a_b.disabled, "disabled" } }) {
			if (on) {
				s += std::format(", {}", name);
			}
		}
		s += std::format(", bond {}, deployed {} times", a_b.bond, a_b.calls);
		return s;
	}

	bool Sick(const SaveBits& a_b) { return a_b.dead || a_b.hospitalize || a_b.dying || a_b.disabled; }

	SaveState Classify(const SaveBits& a_b)
	{
		if (!a_b.known) {
			return SaveState::kUnknown;
		}
		if (a_b.lost || a_b.didLost) {
			return SaveState::kGone;
		}
		if (a_b.obtained) {
			return a_b.sortie ? SaveState::kPresent : SaveState::kRecruited;
		}
		return a_b.killed ? SaveState::kKilled : SaveState::kNever;
	}

	const char* StateName(SaveState a_s)
	{
		switch (a_s) {
		case SaveState::kPresent:
			return "present";
		case SaveState::kRecruited:
			return "recruited, not deployable yet";
		case SaveState::kGone:
			return "gone";
		case SaveState::kNever:
			return "never recruited";
		case SaveState::kKilled:
			return "killed";
		default:
			return "unknown";
		}
	}

	bool BorrowWanted(SaveState a_state, const SaveBits& a_b, BorrowMode a_mode)
	{
		if (a_mode == BorrowMode::kOff) {
			return false;
		}
		switch (a_state) {
		case SaveState::kPresent:
			return Sick(a_b);  // sickbay / dying / dead: a Quiet the save has
		case SaveState::kRecruited:
		case SaveState::kGone:
			return true;
		case SaveState::kNever:
		case SaveState::kKilled:
			return a_mode == BorrowMode::kAlways;
		default:
			return false;
		}
	}

	std::vector<std::string> BorrowPlan(const SaveBits& a_b, BorrowMode a_mode)
	{
		std::vector<std::string> out;
		const auto               state = Classify(a_b);
		if (!BorrowWanted(state, a_b, a_mode)) {
			return out;
		}
		constexpr const char* kObtain = "TppBuddyService.SetObtainedBuddyType(BuddyType.QUIET)";
		constexpr const char* kSortie = "TppBuddyService.SetSortieBuddyType(BuddyType.QUIET)";
		switch (state) {
		case SaveState::kRecruited:
			out.emplace_back(kSortie);
			break;
		case SaveState::kGone:
			if (!a_b.obtained) {
				out.emplace_back(kObtain);
			}
			if (!a_b.sortie) {
				out.emplace_back(kSortie);
			}
			if (a_b.lost) {
				out.emplace_back("TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)");
			}
			break;
		case SaveState::kNever:
		case SaveState::kKilled:
			out.emplace_back(kObtain);
			out.emplace_back(kSortie);
			break;
		default:
			break;
		}
		// Each of these only when that bit is set (never BOSS_QUIET_KILL, never the bond, never a gvar).
		if (a_b.hospitalize) {
			out.emplace_back("TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_HOSPITALIZE)");
		}
		if (a_b.dying) {
			out.emplace_back("TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_DYING)");
		}
		if (a_b.dead) {
			out.emplace_back("TppBuddyService.UnsetDeadBuddyType(BuddyType.QUIET)");
		}
		if (a_b.disabled) {
			out.emplace_back("TppBuddyService.UnsetDisableBuddyType(BuddyType.QUIET)");
		}
		return out;
	}

	std::vector<std::string> RestorePlan(const SaveBits& a_snap, const SaveBits& a_now)
	{
		std::vector<std::string> out;
		if (!a_snap.known || !a_now.known) {
			return out;
		}
		const auto bit = [&](bool a_was, bool a_is, const char* a_set, const char* a_unset) {
			if (a_was != a_is) {
				out.emplace_back(a_was ? a_set : a_unset);
			}
		};
		bit(a_snap.obtained, a_now.obtained, "TppBuddyService.SetObtainedBuddyType(BuddyType.QUIET)", "TppBuddyService.UnsetObtainedBuddyType(BuddyType.QUIET)");
		bit(a_snap.sortie, a_now.sortie, "TppBuddyService.SetSortieBuddyType(BuddyType.QUIET)", "TppBuddyService.UnsetSortieBuddyType(BuddyType.QUIET)");
		bit(a_snap.lost, a_now.lost, "TppBuddyService.SetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)",
			"TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_LOST)");
		bit(a_snap.hospitalize, a_now.hospitalize, "TppBuddyService.SetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_HOSPITALIZE)",
			"TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_HOSPITALIZE)");
		bit(a_snap.dying, a_now.dying, "TppBuddyService.SetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_DYING)",
			"TppBuddyService.UnsetBuddyCommonFlag(BuddyCommonFlag.BUDDY_QUIET_DYING)");
		bit(a_snap.dead, a_now.dead, "TppBuddyService.SetDeadBuddyType(BuddyType.QUIET)", "TppBuddyService.UnsetDeadBuddyType(BuddyType.QUIET)");
		bit(a_snap.disabled, a_now.disabled, "TppBuddyService.SetDisableBuddyType(BuddyType.QUIET)", "TppBuddyService.UnsetDisableBuddyType(BuddyType.QUIET)");
		return out;
	}

	// ---- the raw block ------------------------------------------------------------------------------------------------------------
	namespace
	{
		struct Field
		{
			std::uint32_t first, end;  // [first, end)
			const char*   name;
			std::uint8_t  quietMask;   // per byte: 0xFF whole byte, a bit mask, or 0 (not hers)
			std::uint32_t quietFirst{ 0 }, quietEnd{ 0 };  // the whole-byte range that is hers within an array (else none)
		};
		// vars_registrations_1.0.15.4.txt: the buddy vars from 0x2D08 to 0x2E48.
		constexpr Field kFields[] = {
			{ 0x2D08, 0x2D09, "buddyTypeMissionStart", 0 },
			{ 0x2D09, 0x2D0A, "obtainedBuddyTypeMissionStart", kQuietBit },
			{ 0x2D0A, 0x2D0B, "sortieBuddyTypeMissionStart", kQuietBit },
			{ 0x2D0B, 0x2D0C, "deadBuddyTypeMissionStart", kQuietBit },
			{ 0x2D0C, 0x2D10, "initialBuddyStatusMissionStart", 0 },
			{ 0x2D10, 0x2D1C, "initialBuddyPosMissionStart", 0 },
			{ 0x2D1C, 0x2D20, "initialBuddyRotYMissionStart", 0 },
			{ 0x2D20, 0x2D24, "buddyCommonFlagMissionStart", 0 },  // bits 0-4 of its low byte: below
			{ 0x2D24, 0x2D34, "buddyFriendlyPointMissionStart", 0, 0x2D24, 0x2D26 },
			{ 0x2D34, 0x2D68, "(between)", 0 },
			{ 0x2D68, 0x2D69, "buddyType", 0 },
			{ 0x2D69, 0x2D6A, "obtainedBuddyType", kQuietBit },
			{ 0x2D6A, 0x2D6B, "sortieBuddyType", kQuietBit },
			{ 0x2D6B, 0x2D6C, "disableCallBuddyType", kQuietBit },
			{ 0x2D6C, 0x2D6D, "deadBuddyType", kQuietBit },
			{ 0x2D6D, 0x2D70, "initialBuddyStatus", 0 },
			{ 0x2D70, 0x2D7C, "initialBuddyPos", 0 },
			{ 0x2D7C, 0x2D80, "initialBuddyRotY", 0 },
			{ 0x2D80, 0x2D84, "buddyCommonFlag", 0 },  // bits 0-4: below
			{ 0x2D84, 0x2D88, "buddyMissionFlag", 0 },
			{ 0x2D88, 0x2DA4, "buddyCallCount", 0, 0x2D94, 0x2D98 },  // [3] = QUIET (u32 x 7)
			{ 0x2DA4, 0x2DC4, "buddyEspionageCpNames", 0 },
			{ 0x2DC4, 0x2DE4, "buddyFriendlyTimer", 0, 0x2DC4, 0x2DC8 },  // [0] = BuddyFriendlyType.QUIET (u32 x 8)
			{ 0x2DE4, 0x2DF4, "buddyFriendlyPoint", 0, 0x2DE4, 0x2DE6 },  // u16 x 8
			{ 0x2DF4, 0x2E04, "buddyFriendlyMaxPoint", 0, 0x2DF4, 0x2DF6 },
			// u8 x 64: 8 per BuddyFriendlyType (QUIET 0 = [0..7]): live M1, only Quiet acted and [2] and [5] moved with her hits
			{ 0x2E04, 0x2E44, "buddyFriendlyCount", 0, 0x2E04, 0x2E0C },
			{ 0x2E44, 0x2E46, "buddyQuietEquipType", 0xFF },
			{ 0x2E46, 0x2E47, "buddyQuietCostumeType", 0xFF },
			{ 0x2E47, 0x2E48, "buddyDogFlag", 0 },
		};
	}

	std::uint8_t QuietMaskAt(std::uint32_t a_offset)
	{
		if (a_offset == 0x2D20 || a_offset == 0x2D80) {
			return 0x1F;  // BOSS_QUIET_KILL, DYING, HOSPITALIZE, LOST, VANISH
		}
		for (const auto& f : kFields) {
			if (a_offset >= f.first && a_offset < f.end) {
				if (f.quietEnd > f.quietFirst) {
					return a_offset >= f.quietFirst && a_offset < f.quietEnd ? 0xFF : 0;
				}
				return f.quietMask;
			}
		}
		return 0;
	}

	const char* FieldAt(std::uint32_t a_offset)
	{
		for (const auto& f : kFields) {
			if (a_offset >= f.first && a_offset < f.end) {
				return f.name;
			}
		}
		return "?";
	}

	std::vector<std::pair<std::uint32_t, std::uint32_t>> CounterSpans()
	{
		// Whole bytes of hers the put-back writes back raw: the call count, the friendly timer / point / max point and the point's
		// mission-start copy, her equipment and costume. (The bits go back through Lua's setters: RestorePlan.)
		return { { 0x2D94, 4 }, { 0x2DC4, 4 }, { 0x2DE4, 2 }, { 0x2DF4, 2 }, { 0x2D24, 2 }, { 0x2E04, 8 }, { 0x2E44, 2 }, { 0x2E46, 1 } };
	}

	std::vector<std::pair<std::uint32_t, std::uint8_t>> MaskedSpans()
	{
		return { { 0x2D09, kQuietBit }, { 0x2D0A, kQuietBit }, { 0x2D0B, kQuietBit }, { 0x2D20, 0x1F }, { 0x2D69, kQuietBit }, { 0x2D6A, kQuietBit },
			{ 0x2D6B, kQuietBit }, { 0x2D6C, kQuietBit }, { 0x2D80, 0x1F } };
	}

	std::vector<ByteDiff> DiffBlock(const std::uint8_t* a_snap, const std::uint8_t* a_now, std::uint8_t a_staticSnap, std::uint8_t a_staticNow)
	{
		std::vector<ByteDiff> out;
		for (std::uint32_t i = 0; i < kBlockBytes; ++i) {
			if (a_snap[i] == a_now[i]) {
				continue;
			}
			const auto off = kBlockFirst + i;
			const auto mask = QuietMaskAt(off);
			out.push_back({ off, a_snap[i], a_now[i], ((a_snap[i] ^ a_now[i]) & mask) != 0, FieldAt(off) });
		}
		if (a_staticSnap != a_staticNow) {
			out.push_back({ 0xFFFFFFFFu, a_staticSnap, a_staticNow, ((a_staticSnap ^ a_staticNow) & kQuietBit) != 0, "SetDisableBuddyType's static byte" });
		}
		return out;
	}

	bool DiffClean(const std::vector<ByteDiff>& a_diff)
	{
		return std::none_of(a_diff.begin(), a_diff.end(), [](const ByteDiff& d) { return d.quiet; });
	}

	std::string DescribeDiff(const std::vector<ByteDiff>& a_diff)
	{
		if (a_diff.empty()) {
			return "no byte differs";
		}
		std::string s;
		for (const auto& d : a_diff) {
			s += std::format("{}{} {} {:#04x}->{:#04x}{}", s.empty() ? "" : "; ", d.offset == 0xFFFFFFFFu ? std::string("static") : std::format("+{:#x}", d.offset), d.field,
				d.before, d.after, d.quiet ? " QUIET'S" : "");
		}
		return s;
	}

	// ---- her weapon ---------------------------------------------------------------------------------------------------------------
	proto::QuietWeapon WeaponOfAttack(std::uint32_t a_attackId)
	{
		switch (a_attackId) {
		case kAtkWicked:
			return proto::kQuietLethal;
		case kAtkGuilty:
			return proto::kQuietTranq;
		case kAtkSinful:
			return proto::kQuietHeavy;
		default:
			return proto::kQuietWeaponUnknown;
		}
	}

	std::uint32_t AttackOfWeapon(proto::QuietWeapon a_w)
	{
		switch (a_w) {
		case proto::kQuietLethal:
			return kAtkWicked;
		case proto::kQuietTranq:
			return kAtkGuilty;
		case proto::kQuietHeavy:
			return kAtkSinful;
		default:
			return 0;
		}
	}

	proto::QuietWeapon WeaponOfEquip(int a_equip)
	{
		// [V] 4 = SINFUL BUTTERFLY (the player's save; her hit carried 292). The rest by the equip table's order (Q11).
		switch (a_equip) {
		case 0:
		case 1:
			return proto::kQuietLethal;
		case 2:
		case 3:
			return proto::kQuietTranq;
		case 4:
		case 5:
			return proto::kQuietHeavy;
		default:
			return proto::kQuietWeaponUnknown;
		}
	}

	const char* WeaponName(proto::QuietWeapon a_w)
	{
		switch (a_w) {
		case proto::kQuietLethal:
			return "Wicked Butterfly (lethal)";
		case proto::kQuietTranq:
			return "Guilty Butterfly (tranq)";
		case proto::kQuietHeavy:
			return "Sinful Butterfly (heavy)";
		default:
			return "unknown rifle";
		}
	}

	proto::QuietWeapon ShotWeapon(std::uint32_t a_attackId, proto::QuietWeapon a_equip)
	{
		const auto w = a_attackId ? WeaponOfAttack(a_attackId) : proto::kQuietWeaponUnknown;
		return w != proto::kQuietWeaponUnknown ? w : a_equip;
	}

	bool OrderRound(std::uint32_t a_attackId, proto::QuietWeapon a_equip)
	{
		if (a_attackId == 141 || a_attackId == 142) {
			return false;  // her dash attacks (ATK 141 / 142): the same damage message, no rifle round
		}
		return ShotWeapon(a_attackId, a_equip) != proto::kQuietWeaponUnknown;
	}

	const char* PhaseName(proto::QuietPhase a_p)
	{
		switch (a_p) {
		case proto::kQuietOrdered:
			return "Ordered";
		case proto::kQuietHit:
			return "Hit";
		case proto::kQuietReleased:
			return "Released";
		case proto::kQuietRefused:
			return "Refused";
		case proto::kQuietLost:
			return "Lost";
		default:
			return "?";
		}
	}

	CommandUse UseOf(int a_cmd, const TakeOver& a_t)
	{
		if (!IsQuietCommand(a_cmd) || !a_t.layer) {
			return CommandUse::kPass;
		}
		if (IsOrderCommand(a_cmd)) {
			return a_t.orders ? CommandUse::kJob : CommandUse::kPass;
		}
		if (a_cmd == kCmdCover) {
			return a_t.cover ? CommandUse::kPass : CommandUse::kRefuse;
		}
		if (a_cmd == kCmdTalk) {
			return a_t.talk ? CommandUse::kRefuse : CommandUse::kPass;
		}
		return CommandUse::kPass;  // 5 Cease cover, 6 Shoot this (her fire is off outside orders anyway)
	}

	// ---- the order job ------------------------------------------------------------------------------------------------------------
	const char* StageName(OrderJob::Stage a_s)
	{
		switch (a_s) {
		case OrderJob::Stage::kPick:
			return "pick";
		case OrderJob::Stage::kRealize:
			return "realize";
		case OrderJob::Stage::kOrdered:
			return "ordered";
		case OrderJob::Stage::kHit:
			return "hit";
		default:
			return "idle";
		}
	}

	JobStep OrderJob::Start(int a_cmd, std::uint64_t a_nowMs)
	{
		*this = {};
		m_stage = Stage::kPick;
		m_cmd = a_cmd;
		m_startMs = a_nowMs;
		return { .why = "the player's order" };
	}

	JobStep OrderJob::Update(const JobInput& a_in, const JobSettings& a_s)
	{
		JobStep step;
		switch (m_stage) {
		case Stage::kIdle:
			break;
		case Stage::kPick:
			if (a_in.cancel) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietRefused;
				step.why = "cancelled before a pick";
			} else if (a_in.pick) {
				m_handle = a_in.pick;
				m_realizeMs = a_in.nowMs;
				m_stage = Stage::kRealize;
				step.dedicate = true;
				step.why = "the host's pick";
			} else if (a_in.nowMs - m_startMs >= a_s.pickWaitMs) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietRefused;
				step.why = "no target -- no enemy picked";
			}
			break;
		case Stage::kRealize:
			if (a_in.cancel || !a_in.targetPresent) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietRefused;
				step.release = true;
				step.why = a_in.cancel ? "cancelled before the order" : "the target left before the order";
			} else if (a_in.proxyReady) {
				m_stage = Stage::kOrdered;
				m_orderMs = a_in.nowMs;
				step.order = true;
				step.event = proto::kQuietOrdered;
				step.why = "MGSV ordered on his stand-in";
			} else if (a_in.nowMs - m_realizeMs >= a_s.realizeTimeoutMs) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietRefused;
				step.release = true;
				step.why = "his stand-in wasn't ready in time";
			}
			break;
		case Stage::kOrdered:
			if (a_in.hit) {
				m_stage = Stage::kHit;
				step.event = proto::kQuietHit;
				step.why = "her round hit his stand-in";
			} else if (a_in.cancel) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietLost;
				step.lost = proto::kQuietLostCancelled;
				step.release = true;
				step.why = "cancelled";
			} else if (!a_in.targetPresent) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietLost;
				step.lost = proto::kQuietLostTarget;
				step.release = true;
				step.why = "the target died or left";
			} else if (a_in.cant || a_in.nowMs - m_orderMs >= (Aim() ? a_s.aimHoldMs : a_s.hitTimeoutMs)) {
				m_stage = Stage::kIdle;
				step.event = proto::kQuietLost;
				step.lost = proto::kQuietLostMgsv;
				step.release = true;
				step.why = a_in.cant ? "MGSV said she can't" : "no hit in time";
			}
			break;
		case Stage::kHit:
			m_stage = Stage::kIdle;
			step.event = proto::kQuietReleased;
			step.release = true;
			step.why = "the order is over";
			break;
		}
		return step;
	}

	JobStep OrderJob::Abort(const char* a_why)
	{
		JobStep step;
		step.why = a_why;
		switch (m_stage) {
		case Stage::kOrdered:
			step.event = proto::kQuietLost;
			step.lost = proto::kQuietLostCancelled;
			step.release = true;
			break;
		case Stage::kHit:
			step.event = proto::kQuietReleased;
			step.release = true;
			break;
		case Stage::kPick:
			step.event = proto::kQuietRefused;
			break;
		case Stage::kRealize:
			step.event = proto::kQuietRefused;
			step.release = true;
			break;
		default:
			break;
		}
		m_stage = Stage::kIdle;
		return step;
	}

	bool WantStandIn(std::uint64_t a_pick, bool a_binoculars, std::uint64_t a_lastBinocularsMs, std::uint64_t a_nowMs, std::uint64_t a_keepMs)
	{
		if (!a_pick) {
			return false;
		}
		return a_binoculars || (a_lastBinocularsMs && a_nowMs >= a_lastBinocularsMs && a_nowMs - a_lastBinocularsMs <= a_keepMs);
	}

	// ---- the grace sit -----------------------------------------------------------------------------------------------------------
	float SitYaw(const Vec3& a_centre, const Vec3& a_seat, float a_seatYaw, SitPose a_pose)
	{
		if (a_pose != SitPose::kWatch) {
			return a_seatYaw;
		}
		const Vec3 out = a_seat - a_centre;
		if (!a_centre.Finite() || out.LengthXZ() < 0.05f) {
			return std::remainder(a_seatYaw + std::numbers::pi_v<float>, 2.0f * std::numbers::pi_v<float>);
		}
		return std::atan2(out.x, out.z);
	}

	SitStep SitHold::Update(const SitInput& a_in, SitPose a_pose)
	{
		SitStep step;
		const bool seatOk = a_in.atGrace && a_in.graceSeq != 0 && a_in.seat.Finite();
		if (!a_in.out || !a_in.canRest || !a_in.atGrace) {
			if (m_want || m_seated) {
				step.getUp = m_ordered;
				step.why = !a_in.atGrace ? "Snake left the grace" : !a_in.out ? "Quiet is not out" : "RestAtGrace off";
			}
			const auto lastSeq = m_lastSeq;
			const auto moveId = m_moveId;
			*this = {};
			m_lastSeq = lastSeq;
			m_moveId = moveId;
			return step;
		}
		if (seatOk && a_in.graceSeq != m_lastSeq) {
			// A new rest with a seat (published only inside the host's observed black): her order to it, then placed there.
			m_lastSeq = a_in.graceSeq;
			m_want = a_in.graceSeq;
			m_seated = 0;
			m_tries = 0;
			m_seat = a_in.seat;
			m_yaw = SitYaw(a_in.centre, a_in.seat, a_in.seatYaw, a_pose);
			m_stage = Stage::kStart;
		}
		if (!m_want || !a_in.placeable || !a_in.her) {
			return step;  // no one-frame method now: nothing (the host keeps her hidden past its deadline)
		}
		const float off = (*a_in.her - m_seat).LengthXZ();
		if (!m_haveLast || (*a_in.her - m_last).Length() > 0.03f || !m_stillSince) {
			m_stillSince = a_in.nowMs ? a_in.nowMs : 1;
		}
		m_last = *a_in.her;
		m_haveLast = true;
		const auto  order = [&](const char* a_why) {
			step.moveTo = m_seat;
			step.moveAim = a_pose == SitPose::kWatch;
			step.moveId = ++m_moveId;
			m_ordered = true;
			m_stage = Stage::kMove;
			m_tries = 0;
			step.why = a_why;
		};
		// Her order goes out from 2.2-3.5 m off the seat (MoveToPosition ignores a point within 2 m; from farther she runs there and plays a
		// long arrival -- a leap and a landing -- after she has been placed: live 0.9.0 integration, Rampart Tower, from her post 47 m away
		// the leap played 1.5 s into the grace menu). So she is put 2.5 m off the seat first unless she already stands there (M1 measured
		// that case: a short arrival, the rifle down ~0.4 s after the placement).
		const auto orderable = [](float a_off) { return a_off >= 2.2f && a_off <= kSitOrderFar; };
		switch (m_stage) {
		case Stage::kStart:
			if (!orderable(off)) {
				// 2.5 m off first, away from the grace through the seat.
				Vec3 dir = m_seat - (a_in.centre.Finite() ? a_in.centre : *a_in.her);
				dir.y = 0.0f;
				if (dir.LengthXZ() < 0.1f) {
					dir = { 0.0f, 0.0f, 1.0f };
				}
				m_away = m_seat + dir * (2.5f / dir.LengthXZ());
				step.place = m_away;
				step.placeYaw = m_yaw;
				step.why = "2.5 m off the seat first";
				m_stage = Stage::kAway;
			} else {
				order("her order to the grace seat");
			}
			break;
		case Stage::kAway:
			if (orderable(off)) {
				order("her order to the grace seat");
			} else if (++m_tries <= 6) {
				step.place = m_away;
				step.placeYaw = m_yaw;
				step.why = "2.5 m off the seat first";
			} else if (off > 2.2f) {
				order("her order to the grace seat (from where she is: the placement 2.5 m off didn't hold)");
			}
			break;
		case Stage::kMove:
			if (a_in.moveSent == m_moveId) {
				step.place = m_seat;
				step.placeYaw = m_yaw;
				step.why = "to the grace seat";
				m_stage = Stage::kPlace;
				m_tries = 0;
				m_orderMs = a_in.nowMs ? a_in.nowMs : 1;
			}
			break;
		case Stage::kPlace: {
			// Seated (echoed) once she has settled: kSitSettleMs after her order and still for kSitStillMs, or kSitSettleMaxMs at the latest.
			const std::uint64_t since = a_in.nowMs - m_orderMs;
			const bool          settled = (since >= kSitSettleMs && a_in.nowMs - m_stillSince >= kSitStillMs) || since >= kSitSettleMaxMs;
			if (off <= 0.3f && settled) {
				m_seated = m_want;
				m_stage = Stage::kHeld;
				step.why = "at the seat (settled)";
			} else if (off > 0.3f && ++m_tries <= 6) {
				step.place = m_seat;
				step.placeYaw = m_yaw;
				step.why = "to the grace seat";
			}
			break;
		}
		case Stage::kHeld:
			// Held there: re-placed if she drifts over 0.3 m (one-frame method only).
			if (off > 0.3f && ++m_tries <= 12) {
				step.place = m_seat;
				step.placeYaw = m_yaw;
				step.why = "drifted from the seat";
			}
			break;
		default:
			break;
		}
		return step;
	}

	// ---- the mirror ---------------------------------------------------------------------------------------------------------------
	float RegionRadius(const std::vector<float>& a_radii, float a_bearing)
	{
		if (a_radii.empty()) {
			return 0.0f;
		}
		if (a_radii.size() == 1) {
			return a_radii[0];
		}
		const float two = 2.0f * std::numbers::pi_v<float>;
		float       b = std::fmod(a_bearing, two);
		if (b < 0.0f) {
			b += two;
		}
		const float step = two / static_cast<float>(a_radii.size());
		const float f = b / step;
		const auto  i = static_cast<std::size_t>(std::floor(f)) % a_radii.size();
		const auto  j = (i + 1) % a_radii.size();
		const float t = f - std::floor(f);
		return a_radii[i] * (1.0f - t) + a_radii[j] * t;
	}

	bool InRegion(const Vec3& a_p, const Vec3& a_centre, const std::vector<float>& a_radii)
	{
		const Vec3 d = a_p - a_centre;
		return d.LengthXZ() <= RegionRadius(a_radii, std::atan2(d.x, d.z)) + 1e-3f;
	}

	Vec3 MirrorPoint(const Vec3& a_want, const Vec3& a_snake, const Vec3& a_cam, const Vec3& a_centre, const std::vector<float>& a_radii, float a_groundY,
		float a_minSnake)
	{
		Vec3 p{ a_want.x, a_groundY, a_want.z };
		if (!a_radii.empty() && !InRegion(p, a_centre, a_radii)) {
			// Along MGSV's camera line: the camera's own point (or Snake's, when the camera is outside) inward, bisected to the boundary.
			Vec3 from{ a_cam.x, a_groundY, a_cam.z };
			if (!a_cam.Finite() || !InRegion(from, a_centre, a_radii)) {
				from = { a_snake.x, a_groundY, a_snake.z };
			}
			float lo = 0.0f, hi = 1.0f;
			for (int k = 0; k < 24; ++k) {
				const float mid = 0.5f * (lo + hi);
				const Vec3  q = from + (p - from) * mid;
				(InRegion(q, a_centre, a_radii) ? lo : hi) = mid;
			}
			p = from + (p - from) * lo;
			p.y = a_groundY;
		}
		const Vec3  fromSnake{ p.x - a_snake.x, 0.0f, p.z - a_snake.z };
		const float d = fromSnake.LengthXZ();
		if (d < a_minSnake) {
			// Too near Snake: pushed out along the camera's line through the point (or straight away from him).
			Vec3 dir{ p.x - a_cam.x, 0.0f, p.z - a_cam.z };
			if (!a_cam.Finite() || dir.LengthXZ() < 0.1f) {
				dir = d > 0.05f ? fromSnake : Vec3{ 0.0f, 0.0f, 1.0f };
			}
			const float len = dir.LengthXZ();
			dir = dir * (1.0f / len);
			// Solve |p + dir*t - snake| = minSnake for the smallest t >= 0.
			const Vec3  w{ p.x - a_snake.x, 0.0f, p.z - a_snake.z };
			const float b = w.x * dir.x + w.z * dir.z;
			const float c = w.x * w.x + w.z * w.z - a_minSnake * a_minSnake;
			const float t = -b + std::sqrt(std::max(0.0f, b * b - c));
			p = p + dir * t;
			p.y = a_groundY;
		}
		return p;
	}
}
