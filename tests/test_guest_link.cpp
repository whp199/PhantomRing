// Offline test of the MGSV guest's link (mgsv/Link.cpp) against a simulated host on a real file
// mapping: attach handshake, producer reset on host restart, GuestState/PoseBlock/event flow, the '!'
// console, protocol version refusal. Run under wine (target check_guest).
#include "Config.h"
#include "Link.h"
#include "Log.h"
#include "Mapping.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

using namespace phantom;
using namespace phantom::proto;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

static std::int64_t Qpc()
{
	LARGE_INTEGER t;
	::QueryPerformanceCounter(&t);
	return t.QuadPart;
}

template <class F>
static bool WaitFor(F&& a_cond, Header* a_hdr, int a_ms = 3000)
{
	for (int t = 0; t < a_ms; t += 10) {
		Atomic(a_hdr->hostHeartbeatQpc).store(Qpc());
		if (a_cond()) {
			return true;
		}
		::Sleep(10);
	}
	return false;
}

static std::string Command(std::string_view a_line) { return "echo " + std::string(a_line); }

// The shipped mgsv/PhantomRingGuest.ini must state exactly the built-in defaults (it is the documentation).
static void TestShippedIni(const wchar_t* a_path)
{
	using phantom::guest::Config;
	Config shipped;
	shipped.Load(a_path);
	const Config d;
	CHECK(shipped.debugLog == d.debugLog && shipped.mapping == d.mapping);
	CHECK(shipped.autoBoot == d.autoBoot && shipped.mission == d.mission && shipped.hasPosition == d.hasPosition && shipped.rotYDeg == d.rotYDeg);
	CHECK(shipped.disableSoldiers == d.disableSoldiers && shipped.invincible == d.invincible && shipped.clockTime == d.clockTime &&
	      shipped.clearMarkersSec == d.clearMarkersSec && d.clearMarkersSec == 5 && shipped.autoAim == d.autoAim && !d.autoAim);
	CHECK(shipped.freezeClock == d.freezeClock && shipped.weather == d.weather && shipped.pressButtons == d.pressButtons && shipped.timeoutSec == d.timeoutSec);
	CHECK(shipped.buddy == d.buddy && shipped.buddyAhead == d.buddyAhead && shipped.buddySide == d.buddySide && shipped.buddyFollow == d.buddyFollow &&
	      shipped.buddyFollowDistance == d.buddyFollowDistance && shipped.buddyFollowEveryMs == d.buddyFollowEveryMs &&
	      shipped.buddyFollowAhead == d.buddyFollowAhead && shipped.buddyFollowSide == d.buddyFollowSide && shipped.buddyRevive == d.buddyRevive);
	CHECK(shipped.holdMode == d.holdMode && shipped.radius == d.radius && shipped.teleportJump == d.teleportJump && shipped.warpCooldownMs == d.warpCooldownMs);
	CHECK(shipped.treadmillOnVehicle == d.treadmillOnVehicle && shipped.treadmillMovesBuddy == d.treadmillMovesBuddy && shipped.velocitySmoothing == d.velocitySmoothing);
	CHECK(shipped.ridingCameraKeep == d.ridingCameraKeep && d.ridingCameraKeep);
	CHECK(shipped.jobSleepUs == d.jobSleepUs && shipped.waitProbe == d.waitProbe);
	CHECK(shipped.padDuringBoot == d.padDuringBoot && shipped.hideDirectInputControllers == d.hideDirectInputControllers && shipped.rumble == d.rumble &&
	      shipped.guardWindow == d.guardWindow);
	CHECK(shipped.filmHeightMm == d.filmHeightMm && shipped.fovOverrideDeg == d.fovOverrideDeg && shipped.nearPlane == d.nearPlane);
	CHECK(shipped.hurtFeedback == d.hurtFeedback && shipped.hurtBlood == d.hurtBlood && shipped.deathCollapse == d.deathCollapse);
	CHECK(shipped.lifeScale == d.lifeScale && shipped.nativeLifeWrite == d.nativeLifeWrite && shipped.nativeShots == d.nativeShots &&
	      shipped.fireFromAmmo == d.fireFromAmmo && shipped.muzzleBone == d.muzzleBone && shipped.melee == d.melee &&
	      shipped.meleeShapeGeometry == d.meleeShapeGeometry && shipped.tranqWeapons.empty() && shipped.stunWeapons.empty());
	CHECK(shipped.blockSaves == d.blockSaves && shipped.saveSinkDir == d.saveSinkDir && shipped.music == d.music && shipped.ambience == d.ambience && shipped.radio == d.radio);
	CHECK(shipped.luaOverride == d.luaOverride && shipped.dumpDefaultLua == d.dumpDefaultLua);
	CHECK(shipped.capture == d.capture && shipped.hud == d.hud && shipped.hudEvery == d.hudEvery && shipped.warpLift == d.warpLift);
	CHECK(shipped.buddyFollowLeadSec == d.buddyFollowLeadSec && shipped.buddyFollowSteer == d.buddyFollowSteer && shipped.buddyFollowRespectCommands == d.buddyFollowRespectCommands && shipped.blasts == d.blasts && shipped.throws == d.throws && shipped.placed == d.placed && shipped.mgsvBlasts == d.mgsvBlasts && !d.mgsvBlasts &&
	      shipped.buddyFollowSideRunning == d.buddyFollowSideRunning && shipped.buddyFollowCatchUp == d.buddyFollowCatchUp &&
	      shipped.buddyFollowMethod == d.buddyFollowMethod && shipped.buddyFollowModes == d.buddyFollowModes && d.buddyFollowMethod == "record" &&
	      shipped.walkerVolume == d.walkerVolume && shipped.safePauseMenu == d.safePauseMenu && shipped.blockMissions == d.blockMissions &&
	      shipped.loadoutPrimary == d.loadoutPrimary && shipped.loadoutPrimary2 == d.loadoutPrimary2 && shipped.loadoutSecondary == d.loadoutSecondary && shipped.loadoutSupport == d.loadoutSupport && shipped.boxPoster == d.boxPoster && shipped.box == d.box);

	// And a non-default file parses into what it says.
	const std::wstring custom = L"test_guest_custom.ini";
	FILE*              f = _wfopen(custom.c_str(), L"wb");
	std::fputs("[Boot]\nMission = 30010\nPosition = -1.5, 2.25, 300\nRotY = 90\n[Motion]\nHold = native_treadmill\n[Buddy]\nType = Horse\n"
	           "[Combat]\nTranqWeapons = 0x10, 17\n[Audio]\nMusic = keep\n",
		f);
	std::fclose(f);
	Config c;
	c.Load(custom);
	CHECK(c.mission == 30010 && c.hasPosition && c.position[0] == -1.5f && c.position[1] == 2.25f && c.position[2] == 300.0f && c.rotYDeg == 90.0f);
	CHECK(c.holdMode == phantom::guest::HoldMode::kNativeTreadmill && c.buddy == "horse" && c.music == phantom::guest::MusicMode::kKeep);
	CHECK(c.tranqWeapons.size() == 2 && c.tranqWeapons[0] == 0x10 && c.tranqWeapons[1] == 17);
}

// The real guest link (mgsv/Link.cpp) against a live host on a_where -- fake_er.py or the real host
// link -- driven by tools/phantomring/test_plugins.py. Plays a guest that walks forward at 3 m/s with
// D-Walker deployed, fires once a second and loses life on hurt events. Prints one "PEER {json}" line.
static int RunPeer(const std::wstring& a_where, double a_seconds)
{
	guest::link::SetCommandHandler(&Command);
	guest::link::SetStatus(Status::kReady, "MGSV: peer test");
	guest::link::Start(a_where);
	LARGE_INTEGER f;
	::QueryPerformanceFrequency(&f);
	const auto     t0 = Qpc();
	auto           elapsed = [&] { return static_cast<double>(Qpc() - t0) / static_cast<double>(f.QuadPart); };
	std::uint64_t  frame = 0, published = 0, padReads = 0, hurts = 0, fired = 0, hostStates = 0;
	std::uint32_t  lastPacket = 0, packets = 0, hostFlags = 0;
	std::int32_t   maxLY = 0;
	std::uint16_t  life = 6000;
	double         nextFire = 1.0;
	bool           everAttached = false, hostAliveSeen = false;
	static PoseBlock pose{};
	pose.boneCount = 178;
	for (std::uint32_t i = 0; i < pose.boneCount; ++i) {
		pose.parent[i] = static_cast<std::int16_t>(i) - 1;
		pose.bones[i].rot[3] = 1.0f;
		pose.bones[i].pos[1] = 0.01f * static_cast<float>(i);
	}
	GuestState s{};
	while (elapsed() < a_seconds) {
		guest::link::NoteGameTick();
		if (guest::link::Attached()) {
			everAttached = true;
			hostAliveSeen |= guest::link::HostAlive();
			++frame;
			s.frame = frame;
			s.qpc = Qpc();
			s.flags = kGuestReady | kGuestStanding;
			s.odoEpoch = guest::link::AttachGeneration();
			s.odo[2] += 0.05;
			s.rootPos[2] = static_cast<float>(std::fmod(s.odo[2], 10.0));
			s.camQuat[3] = 1.0f;
			s.camFovY = 1.0f;
			s.life = life;
			s.lifeMax = 6000;
			s.buddyType = 4;
			s.buddyObjectId = 0x0402;
			s.buddyFlags = kBuddyPresent | kBuddyPosValid;
			s.buddyPos[0] = 2.0f;
			s.buddyPos[2] = s.rootPos[2] - 1.5f;
			s.vehicleId = 0xFFFF;
			guest::link::PublishState(s);
			pose.frame = frame;
			pose.root[0] = pose.root[5] = pose.root[10] = pose.root[15] = 1.0f;
			pose.root[14] = s.rootPos[2];
			guest::link::PublishPose(pose);
			++published;
			PadState pad{};
			if (guest::link::ReadPad(pad)) {
				++padReads;
				if (pad.packet != lastPacket) {
					lastPacket = pad.packet;
					++packets;
				}
				maxLY = std::max<std::int32_t>(maxLY, pad.thumbLY);
			}
			HostState hs{};
			if (guest::link::ReadHostState(hs)) {
				++hostStates;
				hostFlags |= hs.flags;
			}
			HostEvent he{};
			while (guest::link::PopHostEvent(he)) {
				if (he.type == kHEvHurt) {
					++hurts;
					life = static_cast<std::uint16_t>(std::max<int>(1, life - static_cast<int>(he.a)));
				}
			}
			if (elapsed() >= nextFire) {
				nextFire += 1.0;
				GuestEvent e{};
				e.type = kGEvFire;
				e.a = 0x0123;
				e.b = 1;
				e.v0[1] = 1.5f;
				e.v1[2] = 1.0f;
				fired += guest::link::PushEvent(e);
			}
		}
		::Sleep(16);
	}
	std::printf("PEER {\"attached\": %s, \"hostAlive\": %s, \"generation\": %u, \"published\": %llu, \"padReads\": %llu, \"padPackets\": %u, "
	            "\"maxThumbLY\": %d, \"hostStates\": %llu, \"hostFlags\": %u, \"hurts\": %llu, \"life\": %u, \"fired\": %llu}\n",
		everAttached ? "true" : "false", hostAliveSeen ? "true" : "false", guest::link::AttachGeneration(), static_cast<unsigned long long>(published),
		static_cast<unsigned long long>(padReads), packets, maxLY, static_cast<unsigned long long>(hostStates), hostFlags,
		static_cast<unsigned long long>(hurts), life, static_cast<unsigned long long>(fired));
	std::fflush(stdout);
	::ExitProcess(0);  // the link worker never exits
}

int wmain(int argc, wchar_t** argv)
{
	Log::Init(L"test_guest_link.log", "test");
	if (argc >= 4 && std::wcscmp(argv[1], L"--peer") == 0) {
		return RunPeer(argv[2], _wtof(argv[3]));
	}
	if (argc > 1) {
		TestShippedIni(argv[1]);
	}
	wchar_t temp[MAX_PATH];
	::GetTempPathW(MAX_PATH, temp);
	const std::wstring path = std::wstring(temp) + L"phantomring_guest_link_test.bin";
	::DeleteFileW(path.c_str());

	Mapping host;
	CHECK(host.Open(L"file:" + path, kMappingBytes, true));
	auto* base = host.Base();
	auto* hdr = reinterpret_cast<Header*>(base + kOffHeader);
	LARGE_INTEGER f;
	::QueryPerformanceFrequency(&f);
	hdr->qpcFrequency = f.QuadPart;

	guest::link::SetCommandHandler(&Command);
	guest::link::SetStatus(Status::kTitle, "MGSV: at the title screen");
	guest::link::Start(L"file:" + path);

	// Not stamped yet: the guest maps the file, writes its status, but doesn't attach.
	CHECK(WaitFor([&] { return std::strcmp(hdr->guestStatusText, "MGSV: at the title screen") == 0; }, hdr));
	CHECK(hdr->guestStatus == Status::kTitle);
	CHECK(!guest::link::Attached());
	CHECK(hdr->guestHeartbeatQpc != 0);

	// Wrong protocol version: refused.
	hdr->version = kVersion + 1;
	hdr->sessionNonce = 0x1111;
	Atomic(hdr->magic).store(kMagic);
	::Sleep(300);
	CHECK(hdr->guestNonce == 0);
	CHECK(!guest::link::Attached());

	// Right version: attaches, resets its producer sides, sends kRenClearAll.
	const auto gen0 = guest::link::AttachGeneration();
	hdr->version = kVersion;
	CHECK(WaitFor([&] { return hdr->guestNonce == 0x1111; }, hdr));
	CHECK(guest::link::Attached());
	CHECK(guest::link::HostAlive());
	CHECK(guest::link::AttachGeneration() == gen0 + 1);
	auto*              ren = base + kOffRenderRing;
	const std::uint64_t renHead = *reinterpret_cast<std::uint64_t*>(ren + kRenRingHeadOff);
	CHECK(renHead == 8);
	RenMsgHeader msg{};
	std::memcpy(&msg, ren + kRenRingDataOff, sizeof(msg));
	CHECK(msg.type == kRenClearAll && msg.payloadBytes == 0);

	// GuestState and PoseBlock reach the host.
	GuestState s{};
	s.frame = 42;
	s.odo[2] = 12.5;
	s.buddyFlags = kBuddyPresent | kBuddyPosValid;
	s.buddyPos[0] = 3.0f;
	guest::link::PublishState(s);
	GuestState got{};
	CHECK(SeqRead(reinterpret_cast<GuestState*>(base + kOffGuestState), got));
	CHECK(got.frame == 42 && got.odo[2] == 12.5 && got.buddyFlags == (kBuddyPresent | kBuddyPosValid) && got.buddyPos[0] == 3.0f);
	static PoseBlock pose{};
	pose.boneCount = 178;
	pose.bones[177].pos[1] = 1.75f;
	guest::link::PublishPose(pose);
	static PoseBlock poseGot{};
	CHECK(SeqRead(reinterpret_cast<PoseBlock*>(base + kOffPose), poseGot));
	CHECK(poseGot.boneCount == 178 && poseGot.bones[177].pos[1] == 1.75f);

	// Guest events out, host events in, pad in.
	GuestEvent ge{};
	ge.type = kGEvFire;
	ge.a = 0x1234;
	CHECK(guest::link::PushEvent(ge));
	EntryRing<GuestEvent, kEventRingEntries> gring{ base + kOffGuestEvents };
	GuestEvent                               geGot{};
	CHECK(gring.Pop(geGot) && geGot.type == kGEvFire && geGot.a == 0x1234);
	EntryRing<HostEvent, kEventRingEntries> hring{ base + kOffHostEvents };
	HostEvent                               he{};
	he.type = kHEvHurt;
	he.a = 77;
	CHECK(hring.Push(he));
	HostEvent heGot{};
	CHECK(guest::link::PopHostEvent(heGot) && heGot.type == kHEvHurt && heGot.a == 77);
	PadState pad{};
	pad.flags = kPadConnected;
	pad.buttons = 0x1000;
	SeqWrite(reinterpret_cast<PadState*>(base + kOffPad), pad);
	PadState padGot{};
	CHECK(guest::link::ReadPad(padGot) && padGot.buttons == 0x1000);

	// '!' console lines are answered by the worker; Lua lines are left for the game's Lua pump.
	auto* console = reinterpret_cast<Console*>(base + kOffGuestConsole);
	std::strcpy(console->request, "!status");
	Atomic(console->reqSeq).store(console->reqSeq + 1);
	CHECK(WaitFor([&] { return console->respSeq == console->reqSeq; }, hdr));
	CHECK(std::string(console->response) == "echo !status");
	std::strcpy(console->request, "return 1");
	Atomic(console->reqSeq).store(console->reqSeq + 1);
	::Sleep(200);
	CHECK(console->respSeq != console->reqSeq);

	// Host restart (new nonce): the guest re-attaches, drains stale host events, resends kRenClearAll,
	// and drops the unanswered console request of the old session.
	CHECK(hring.Push(he));
	Atomic(hdr->sessionNonce).store(0x2222);
	CHECK(WaitFor([&] { return hdr->guestNonce == 0x2222; }, hdr));
	CHECK(guest::link::AttachGeneration() == gen0 + 2);
	CHECK(!guest::link::PopHostEvent(heGot));
	CHECK(*reinterpret_cast<std::uint64_t*>(ren + kRenRingHeadOff) == 16);
	CHECK(console->respSeq == console->reqSeq);

	// Host gone (heartbeat stale): HostAlive false, so the pad goes neutral.
	hdr->hostHeartbeatQpc = Qpc() - 10 * f.QuadPart;
	CHECK(!guest::link::HostAlive());

	if (g_failures) {
		std::printf("test_guest_link: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("test_guest_link: all passed (guest unix pid %u)\n", guest::link::UnixPid());
	std::fflush(stdout);
	::ExitProcess(0);  // the link worker never exits
}
