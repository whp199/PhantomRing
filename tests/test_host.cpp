// Offline tests for the host's pure parts (run with wine): console expressions, the anchor math,
// and the kinematic controller against a synthetic world (ground, a wall, a step, a block, a drop).
#include "Anchor.h"
#include "BuddyOrders.h"
#include "Config.h"
#include "Console.h"
#include "ConsoleExpr.h"
#include "Host.h"
#include "Interaction.h"
#include "Kinematic.h"
#include "Link.h"
#include "Revival.h"
#include "Log.h"
#include "MenuWatch.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <numbers>

using namespace phantom::er;

static int g_failures = 0;
#define CHECK(x)                                                          \
	do {                                                                  \
		if (!(x)) {                                                       \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
			++g_failures;                                                 \
		}                                                                 \
	} while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

// ---- synthetic world -------------------------------------------------------------------------
// ground y=0; step 0.3 high for z in [10,12]; block 1.0 high for z in [14,16]; a pit (ground -5)
// for z > 20; a wall x in [5, 5.5] up to y=3 everywhere.
static float GroundAt(float x, float z)
{
	(void)x;
	if (z >= 10 && z <= 12) return 0.3f;
	if (z >= 14 && z <= 16) return 1.0f;
	if (z > 20) return -5.0f;
	return 0.0f;
}
static bool Solid(const Vec3& p) { return p.y < GroundAt(p.x, p.z) || (p.x >= 5.0f && p.x <= 5.5f && p.y < 3.0f); }

static bool Ray(const Vec3& from, const Vec3& delta, Vec3& hit)
{
	if (Solid(from)) {
		hit = from;
		return true;
	}
	constexpr int kSteps = 2000;
	Vec3          prev = from;
	for (int i = 1; i <= kSteps; ++i) {
		const Vec3 p = from + delta * (static_cast<float>(i) / kSteps);
		if (Solid(p)) {
			Vec3 a = prev, b = p;  // refine the surface crossing
			for (int k = 0; k < 30; ++k) {
				const Vec3 m = (a + b) * 0.5f;
				(Solid(m) ? b : a) = m;
			}
			hit = b;
			return true;
		}
		prev = p;
	}
	return false;
}

static void TestExpr()
{
	std::map<std::uint64_t, std::uint64_t> mem = { { 0x1000, 0x2000 }, { 0x2000 + 0x1E508, 0x7000 } };
	ExprEnv env;
	env.symbol = [](std::string_view n) -> std::optional<std::uint64_t> {
		if (n == "wcm") return 0x1000;
		if (n == "player.pos") return 0x7070;
		return std::nullopt;
	};
	env.read64 = [&mem](std::uint64_t a) -> std::optional<std::uint64_t> {
		const auto it = mem.find(a);
		return it == mem.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
	};
	auto ok = [&env](const char* e, std::uint64_t want) {
		const auto r = EvalExpr(e, env);
		if (!r || *r != want) {
			std::printf("  expr '%s' -> %s\n", e, r ? std::to_string(*r).c_str() : r.error().c_str());
		}
		return r && *r == want;
	};
	CHECK(ok("[[wcm]+0x1E508]+0x190", 0x7190));
	CHECK(ok(" [ [ wcm ] + 0x1e508 ] ", 0x7000));
	CHECK(ok("player.pos", 0x7070));
	CHECK(ok("2*3+4", 10));
	CHECK(ok("(1+2)*3", 9));
	CHECK(ok("0x10 - 1", 15));
	CHECK(ok("-1", ~0ull));
	CHECK(ok("wcm - -16", 0x1010));
	CHECK(!EvalExpr("nope+1", env));
	CHECK(!EvalExpr("[0x1234]", env));
	CHECK(!EvalExpr("[1+2", env));
	CHECK(!EvalExpr("1 2", env));
	CHECK(!EvalExpr("", env));
	CHECK(!EvalExpr("0xZZ", env));
	CHECK(ParseNumber("1.5") && *ParseNumber("1.5") == 1.5);
	CHECK(ParseNumber("-0x10") && *ParseNumber("-0x10") == -16.0);
	CHECK(ParseInteger("0x1E508") && *ParseInteger("0x1E508") == 0x1E508);
	CHECK(!ParseInteger("12a"));
}

static void TestAnchor()
{
	constexpr float pi = std::numbers::pi_v<float>;
	Anchor          a;
	a.pe = { 100, 10, -50 };
	a.pm = { 1, 2, 3 };
	a.delta = pi / 2;
	CHECK(Near(a.PointToEr(a.pm + Vec3{ 0, 0, 1 }), a.pe + Vec3{ 1, 0, 0 }));
	CHECK(Near(a.PointToEr(a.pm + Vec3{ 0, 1.5f, 0 }), a.pe + Vec3{ 0, 1.5f, 0 }));
	for (const float mirror : { 1.0f, -1.0f }) {
		a.axes.mirrorX = mirror;
		const Vec3 p{ 7, -3, 11 };
		CHECK(Near(a.PointToMgsv(a.PointToEr(p)), p, 1e-3f));
		const Vec3 d{ 0.3f, 0.1f, -0.9f };
		CHECK(Near(a.DirToMgsv(a.DirToEr(d)), d, 1e-5f));
	}
	// delta keeps the puppet's facing at start, whatever conventions are configured; a later MGSV
	// turn by t turns the ER heading by t (mirrored: by -t).
	for (const float erFwd : { 1.0f, -1.0f }) {
		for (const float mirror : { 1.0f, -1.0f }) {
			Axes axes;
			axes.erForward = erFwd;
			axes.mirrorX = mirror;
			const float erYaw = 0.7f;
			const Quat  q{ 0, std::sin(erYaw / 2), 0, std::cos(erYaw / 2) };  // what SetYaw(0.7) builds
			const float rootYaw = -2.1f;
			const float delta = ChooseDelta(q, rootYaw, axes);
			const float heading = MgsvHeading(rootYaw, axes) + delta;
			CHECK(Near(WrapPi(ErYawForHeading(heading, axes) - erYaw), 0.0f, 1e-4f));
			const float turned = MgsvHeading(rootYaw + 0.4f, axes) + delta;
			CHECK(Near(WrapPi(turned - heading), 0.4f * mirror, 1e-4f));
			// and DirToEr agrees with the heading mapping
			Anchor b;
			b.axes = axes;
			b.delta = delta;
			const Vec3 fwdM{ std::sin(rootYaw), 0, std::cos(rootYaw) };
			CHECK(Near(WrapPi(Heading(b.DirToEr(fwdM)) - heading), 0.0f, 1e-4f));
		}
	}
	CHECK(Near(WrapPi(3 * pi), pi, 1e-4f) || Near(WrapPi(3 * pi), -pi, 1e-4f));
}

static void TestKinematic()
{
	const RayFn     ray = &Ray;
	KinematicParams params;
	const float     dt = 1.0f / 60.0f;

	{  // flat walk
		KinematicState s;
		s.pos = { 0, 0, 0 };
		s.grounded = true;
		for (int i = 0; i < 50; ++i) {
			KinematicStep(s, { 0, 0, 0.1f }, dt, params, ray);
		}
		CHECK(Near(s.pos.z, 5.0f, 1e-2f) && Near(s.pos.y, 0.0f, 1e-2f) && s.grounded);
	}
	{  // wall stop
		KinematicState s;
		s.pos = { 3, 0, 0 };
		s.grounded = true;
		for (int i = 0; i < 40; ++i) {
			KinematicStep(s, { 0.2f, 0, 0 }, dt, params, ray);
		}
		CHECK(s.pos.x <= 5.0f - params.radius + 0.02f);
		CHECK(s.pos.x >= 5.0f - params.radius - 0.05f);
	}
	{  // slide along the wall
		KinematicState s;
		s.pos = { 4.6f, 0, 0 };
		s.grounded = true;
		KinematicResult last;
		for (int i = 0; i < 30; ++i) {
			last = KinematicStep(s, { 0.1f, 0, 0.1f }, dt, params, ray);
		}
		CHECK(s.pos.x <= 5.0f - params.radius + 0.02f);
		CHECK(s.pos.z > 2.0f);
		CHECK(last.blocked && last.slid);
	}
	{  // step up 0.3
		KinematicState s;
		s.pos = { 0, 0, 9 };
		s.grounded = true;
		bool stepped = false;
		for (int i = 0; i < 20; ++i) {
			stepped |= KinematicStep(s, { 0, 0, 0.1f }, dt, params, ray).stepped;
		}
		CHECK(stepped && Near(s.pos.y, 0.3f, 1e-2f) && s.pos.z > 10.5f);
	}
	{  // block 1.0 high: not climbable
		KinematicState s;
		s.pos = { 0, 0, 13 };
		s.grounded = true;
		for (int i = 0; i < 30; ++i) {
			KinematicStep(s, { 0, 0, 0.1f }, dt, params, ray);
		}
		CHECK(s.pos.z <= 14.0f - params.radius + 0.02f && Near(s.pos.y, 0.0f, 1e-2f));
	}
	{  // walk off a ledge, fall, land
		KinematicState s;
		s.pos = { 0, 0, 19.5f };
		s.grounded = true;
		bool  fell = false, landed = false;
		float landingSpeed = 0;
		for (int i = 0; i < 240 && !landed; ++i) {
			const auto r = KinematicStep(s, { 0, 0, 0.05f }, dt, params, ray);
			fell |= !s.grounded;
			if (r.landed) {
				landed = true;
				landingSpeed = r.landingSpeed;
			}
		}
		CHECK(fell && landed && Near(s.pos.y, -5.0f, 1e-2f) && s.vy == 0.0f);
		CHECK(landingSpeed > 8.0f && landingSpeed < 12.0f);  // sqrt(2*9.81*5) ~ 9.9 m/s
	}
	{  // no movement, no drift
		KinematicState s;
		s.pos = { 1, 0, 1 };
		s.grounded = true;
		for (int i = 0; i < 100; ++i) {
			KinematicStep(s, { 0, 0, 0 }, dt, params, ray);
		}
		CHECK(Near(s.pos, Vec3{ 1, 0, 1 }, 1e-3f));
	}
	{  // boxed in: open floor 0 directions, beside the wall 3, inside something 8; back to where it stood free
		CHECK(Enclosed({ 1, 0, 1 }, 0.8f, params, ray) == 0 && Enclosed({ 4.6f, 0, 1 }, 0.8f, params, ray) == 3);
		const RayFn cage = [](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
			a_hit = a_from + a_delta * 0.5f;
			return true;
		};
		CHECK(Enclosed({ 0, 0, 0 }, 0.8f, params, cage) == 8);
		// Only the waist ray blocked all round (live: Snake inside a table-high plank, every move refused): boxed in too.
		const float waist = std::max(params.stepUp + 0.1f, params.waistHeight);
		const RayFn plank = [waist](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
			if (std::fabs(a_from.y - waist) > 0.01f) {
				return false;
			}
			a_hit = a_from + a_delta * 0.1f;
			return true;
		};
		CHECK(Enclosed({ 0, 0, 0 }, 0.8f, params, plank) == 8);
		Unstick u;
		for (int i = 0; i < 10; ++i) {  // walks free along x
			CHECK(!u.Update({ 0.1f * static_cast<float>(i), 0, 0 }, 0, true, 0.1 * i, 0.5f));
		}
		CHECK(!u.Update({ 3, 0, 0 }, 8, true, 1.0, 0.5f) && !u.Update({ 3, 0, 0 }, 8, true, 1.3, 0.5f));  // in a cart, not long yet
		CHECK(!u.Update({ 3, 0, 0 }, 6, true, 1.4, 0.5f) && !u.Update({ 3, 0, 0 }, 8, true, 1.5, 0.5f));  // a corner restarts the wait
		auto back = u.Update({ 3, 0, 0 }, 8, true, 2.1, 0.5f);
		CHECK(back && Near(back->x, 0.9f, 1e-3f) && u.Count() == 1);
		u.Shift({ 10, 0, 0 });  // the world moved: so did where it stood
		CHECK(!u.Update({ 13, 0, 0 }, 8, false, 3.0, 0.5f));
		back = u.Update({ 13, 0, 0 }, 8, false, 3.6, 0.5f);
		CHECK(back && Near(back->x, 10.9f, 1e-3f) && u.Count() == 2);
	}
}

// Link + console against a scratch mapping, with this test playing the guest. No game here: the
// address table is empty, so everything game-side must answer "unavailable" instead of crashing.
static void TestLinkAndConsole()
{
	using namespace phantom;
	std::remove("test_link.bin");
	auto& link = Link::Get();
	CHECK(link.Open(L"file:test_link.bin"));
	if (!link.Valid()) {
		return;
	}
	Mapping view;  // the "guest" side: a second view of the same file
	CHECK(view.Open(L"file:test_link.bin", proto::kMappingBytes, false));
	auto* h = reinterpret_cast<proto::Header*>(view.Base());
	CHECK(h->magic == proto::kMagic && h->version == proto::kVersion && h->sessionNonce == link.Nonce() && h->sessionNonce != 0);

	link.Tick();
	CHECK(!link.GuestAlive());
	h->guestNonce = h->sessionNonce;
	h->guestUnixPid = 4242;
	h->guestHeartbeatQpc = Link::Qpc();
	link.Tick();
	CHECK(link.GuestAlive() && link.AttachCount() == 1);
	h->guestNonce = 1;  // a guest from another session doesn't count
	link.Tick();
	CHECK(!link.GuestAlive());
	h->guestNonce = h->sessionNonce;
	link.Tick();
	CHECK(link.GuestAlive());

	// guest state + events
	proto::GuestState gs{};
	gs.flags = proto::kGuestReady;
	gs.buddyType = 4;
	SeqWrite(reinterpret_cast<proto::GuestState*>(view.Base() + proto::kOffGuestState), gs);
	proto::GuestState got{};
	CHECK(link.ReadGuestState(got) && got.buddyType == 4);
	EntryRing<proto::GuestEvent, proto::kEventRingEntries> ring{ view.Base() + proto::kOffGuestEvents };
	proto::GuestEvent ev{};
	ev.type = proto::kGEvLog;
	ev.a = 7;
	CHECK(ring.Push(ev));
	proto::GuestEvent popped{};
	CHECK(link.PopGuestEvent(popped) && popped.a == 7 && !link.PopGuestEvent(popped));

	// host console: peek/poke of our own memory works without the game; game symbols are unavailable
	static volatile std::uint32_t target = 0x12345678;
	auto*       con = reinterpret_cast<proto::Console*>(view.Base() + proto::kOffHostConsole);
	const Frame frame{};
	auto ask = [&](const std::string& a_cmd, bool a_wantError) {
		std::snprintf(con->request, sizeof(con->request), "%s", a_cmd.c_str());
		Atomic(con->reqSeq).store(con->reqSeq + 1, std::memory_order_release);
		Console::Poll(frame);
		const bool served = con->respSeq == con->reqSeq;
		if (!served || (con->status != 0) != a_wantError) {
			std::printf("  console '%s' -> served %d status %u: %s\n", a_cmd.c_str(), served, con->status, con->response);
		}
		return served && (con->status != 0) == a_wantError ? std::string(con->response) : std::string("<bad>");
	};
	const auto addr = std::format("{:#x}", reinterpret_cast<std::uintptr_t>(&target));
	CHECK(ask("peek " + addr + " u32", false).find("305419896") != std::string::npos);
	CHECK(ask("poke " + addr + " u32 0x55", false) != "<bad>");
	CHECK(target == 0x55);
	CHECK(ask("peek " + addr + " + 0 u8 2", false).find("0x55") != std::string::npos);
	CHECK(ask("peek [0x10]", true) != "<bad>");
	CHECK(ask("peek player.pos", true) != "<bad>");
	CHECK(ask("vt 0x10", true) != "<bad>");
	CHECK(ask("chr", true) != "<bad>");
	CHECK(ask("flag nodead on", true) != "<bad>");
	CHECK(ask("call setpos 1 2 3", true) != "<bad>");
	CHECK(ask("nonsense", true) != "<bad>");
	CHECK(ask("help", false).find("peek") != std::string::npos);
	CHECK(ask("status", false).find("guest") != std::string::npos);
	CHECK(ask("sym", false).find("(unavailable)") != std::string::npos);
	CHECK(ask("buddy", false).find("buddy") != std::string::npos);
	view.Close();
	std::remove("test_link.bin");
}

// The shipped er/PhantomRingHost.ini must state exactly the built-in defaults (it is the documentation).
static void TestShippedIni(const wchar_t* a_path)
{
	Config shipped;
	CHECK(shipped.Load(a_path));
	const Config d;
	CHECK(shipped.mapping == std::wstring(phantom::proto::kDefaultMapping) && shipped.debug == d.debug && shipped.verifySha256 == d.verifySha256);
	CHECK(shipped.input == d.input && shipped.puppet == d.puppet && shipped.camera == d.camera && shipped.console == d.console && shipped.actors == d.actors &&
	      shipped.debugDraw == d.debugDraw && shipped.buddy == d.buddy);
	CHECK(shipped.userIndex == d.userIndex && shipped.allowButtons == d.allowButtons && shipped.menuLearn == d.menuLearn && shipped.menuUiStates.empty() &&
	      shipped.menuLatchMs == d.menuLatchMs && shipped.promptButton == d.promptButton && shipped.interactionHudState == d.interactionHudState &&
	      shipped.interactionMinMs == d.interactionMinMs && shipped.interactionMaxMs == d.interactionMaxMs && shipped.viewHoldMs == d.viewHoldMs && shipped.guestIdroidStartMs == d.guestIdroidStartMs && shipped.releasePrompts == d.releasePrompts && shipped.releaseMs == d.releaseMs &&
	      shipped.startWaitMs == d.startWaitMs && shipped.chordMs == d.chordMs && shipped.restPrompts == d.restPrompts && shipped.menuKeys == d.menuKeys &&
	      shipped.guestMenuOwnsPad == d.guestMenuOwnsPad && shipped.padGlyphDevice == d.padGlyphDevice);
	CHECK(d.promptButton == 0x8000);  // Y: Elden Ring's event action on an Xbox pad (VERIFIED live)
	CHECK(shipped.fallWatchSeconds == d.fallWatchSeconds && shipped.fallWatchDrop == d.fallWatchDrop && shipped.unstick == d.unstick &&
	      shipped.unstickSeconds == d.unstickSeconds && shipped.catchUp == d.catchUp);
	CHECK(shipped.hideMode == d.hideMode && shipped.motion == d.motion && shipped.hitHook == d.hitHook && shipped.noDead == d.noDead &&
	      shipped.noDamage == d.noDamage && shipped.mirrorLife == d.mirrorLife && shipped.forwardHeals == d.forwardHeals &&
	      shipped.killOnGuestDeath == d.killOnGuestDeath);
	CHECK(shipped.maxSpeed == d.maxSpeed && shipped.maxSpeedVehicle == d.maxSpeedVehicle && shipped.teleportDrop == d.teleportDrop &&
	      shipped.resyncDistance == d.resyncDistance && shipped.radius == d.radius && shipped.stepUp == d.stepUp && shipped.stepUpVehicle == d.stepUpVehicle &&
	      shipped.snapDown == d.snapDown && shipped.chestHeight == d.chestHeight && shipped.gravity == d.gravity && shipped.rayFilter == d.rayFilter);
	CHECK(shipped.mirrorX == d.mirrorX && shipped.mgsvYawSign == d.mgsvYawSign && shipped.erForward == d.erForward && shipped.mgsvCamForward == d.mgsvCamForward &&
	      shipped.mgsvRootOffsetY == d.mgsvRootOffsetY);
	CHECK(shipped.cameraTaskGroup == d.cameraTaskGroup && shipped.camForwardRow == d.camForwardRow && shipped.fovScale == d.fovScale &&
	      shipped.fovOffset == d.fovOffset && shipped.writeNear == d.writeNear && shipped.cameraCollision == d.cameraCollision && shipped.syncChrCam == d.syncChrCam && shipped.syncCompassCam == d.syncCompassCam &&
	      shipped.eyeHeight == d.eyeHeight && shipped.eyeHeightVehicle == d.eyeHeightVehicle && shipped.collisionMargin == d.collisionMargin);
	CHECK(shipped.smooth == d.smooth && shipped.smoothDelay == d.smoothDelay && shipped.smoothExtrapolate == d.smoothExtrapolate && shipped.smoothMotion == d.smoothMotion &&
	      shipped.collisionRadius == d.collisionRadius && shipped.collisionEaseOut == d.collisionEaseOut && shipped.verticalSmoothing == d.verticalSmoothing);
	CHECK(shipped.actorRadius == d.actorRadius && shipped.actorMax == d.actorMax && shipped.drawSkeleton == d.drawSkeleton && shipped.drawBuddy == d.drawBuddy &&
	      shipped.drawTracers == d.drawTracers && shipped.tracerSeconds == d.tracerSeconds && shipped.traceRange == d.traceRange);
	CHECK(shipped.ladderPrompts == d.ladderPrompts && shipped.ladderWaitMs == d.ladderWaitMs && shipped.ladderSettleMs == d.ladderSettleMs &&
	      shipped.ladderPressDelayMs == d.ladderPressDelayMs);
	CHECK((d.ladderPrompts == std::vector<int>{ 5000, 5010 }));  // ActionButtonParam "Climb" / "Descend"
	CHECK(shipped.maxSpeedHorse == d.maxSpeedHorse && shipped.stepUpHorse == d.stepUpHorse && shipped.radiusVehicle == d.radiusVehicle &&
	      shipped.radiusHorse == d.radiusHorse && shipped.eyeHeightHorse == d.eyeHeightHorse);
	CHECK(shipped.progression == d.progression && d.progression == "off" && shipped.levelReference == d.levelReference &&
	      shipped.levelExponent == d.levelExponent && shipped.weaponBonus == d.weaponBonus && shipped.progressionMin == d.progressionMin &&
	      shipped.progressionMax == d.progressionMax);
	CHECK(shipped.render == d.render);
	CHECK(shipped.overlay == d.overlay);
}

// An Elden Ring interaction started with the prompt button (er/Interaction.h).
static void TestRevival()
{
	// After YOU DIED: a load always ends the death once the player lives.
	RevivalInput in;
	in.load = true;
	CHECK(!Revived(in));
	in.alive = true;
	CHECK(Revived(in));
	// No load, HUD known: 15 s alive with the HUD up (the revival dialog hides it, so its time never counts).
	in = {};
	in.hudKnown = true;
	in.alive = true;
	in.aliveMs = 14000;
	CHECK(!Revived(in));
	in.aliveMs = 16000;
	CHECK(Revived(in));
	// ER's HUD off: sitting in "Choose revival location" (dead bit clear, alive, not moved) must not count
	// after 15 s, nor after a minute; a revival at a stake moves the player; a long cap ends it anyway.
	in = {};
	in.alive = true;
	in.aliveMs = 60000;
	in.movedM = 0.4f;
	CHECK(!Revived(in));
	in.movedM = 30.0f;
	CHECK(Revived(in));
	in.movedM = 0.0f;
	in.aliveMs = kReviveCapMs + 1;
	CHECK(Revived(in));
	in.alive = false;
	CHECK(!Revived(in));
}

static void TestInteraction()
{
	using phantom::er::Interaction;
	Interaction i;
	Interaction::Input in;
	in.nowMs = 1000;
	CHECK(!i.Update(in));  // nothing pressed
	// A pickup: the HUD stays up; Elden Ring keeps the pad for the minimum, then Snake has it back.
	in.pressed = true;
	CHECK(i.Update(in));
	CHECK(i.ButtonsOnly());  // the HUD is still up: no sticks, bumpers or triggers for Elden Ring yet
	in.pressed = false;
	in.nowMs = 2000;
	CHECK(i.Update(in) && i.ButtonsOnly());
	in.nowMs = 2600;
	CHECK(!i.Update(in) && !i.SawHudHidden() && !i.ButtonsOnly());
	// A grace: the HUD hides for as long as the grace menu is up (minutes), then the stand-up.
	in.pressed = true;
	in.nowMs = 10000;
	CHECK(i.Update(in));
	in.pressed = false;
	CHECK(i.ButtonsOnly());  // the sit-down, before the grace menu hides the HUD
	in.hudHidden = true;
	for (std::uint64_t t = 10000; t < 200000; t += 5000) {
		in.nowMs = t;
		CHECK(i.Update(in));
		CHECK(!i.ButtonsOnly());  // the grace menu: the whole pad
	}
	in.hudHidden = false;
	in.menuOpen = true;  // its map (Elden Ring's menu) while the HUD is back
	in.nowMs = 200100;
	CHECK(i.Update(in));
	in.menuOpen = false;
	in.nowMs = 200200;
	CHECK(!i.Update(in) && i.SawHudHidden());
	// Fast travel from the grace map: the load ends it at once; so does a death, and the cap.
	in.pressed = true;
	in.nowMs = 300000;
	CHECK(i.Update(in));
	in.pressed = false;
	in.hudHidden = true;
	in.inWorld = false;
	in.nowMs = 300100;
	CHECK(!i.Update(in));
	in.inWorld = true;
	in.pressed = true;
	CHECK(i.Update(in));
	in.pressed = false;
	in.dead = true;
	CHECK(!i.Update(in));
	in.dead = false;
	in.pressed = true;
	in.maxMs = 5000;
	in.nowMs = 400000;
	CHECK(i.Update(in));
	in.pressed = false;
	in.nowMs = 405000;
	CHECK(!i.Update(in));
	// A press while loading or dead starts nothing.
	in.maxMs = 0;
	in.pressed = true;
	in.inWorld = false;
	CHECK(!i.Update(in));
	// A press-less start that was no screen (review: Elden Ring's "Auto" HUD): abort ends it at once.
	Interaction k;
	Interaction::Input kin;
	kin.nowMs = 1000;
	kin.pressed = true;
	kin.hudHidden = true;
	CHECK(k.Update(kin));
	kin.pressed = false;
	kin.nowMs = 2000;
	CHECK(k.Update(kin));
	kin.abort = true;
	CHECK(!k.Update(kin) && !k.Active());

	// Elden Ring's "Auto" HUD: explained hides never count; the second unexplained one is it, once.
	using phantom::er::AutoHud;
	AutoHud a;
	CHECK(!a.Update({ true, true }) && a.Unexplained() == 0);   // a press, a menu, a grace...
	CHECK(!a.Update({ false, false }));                        // no hide this frame
	CHECK(!a.Update({ true, false }) && a.Unexplained() == 1 && !a.Detected());  // a one-off (a tutorial)
	CHECK(a.Update({ true, false }) && a.Detected());
	CHECK(!a.Update({ true, false }) && a.Detected());  // reported once, stays on

}

// D-Walker's orders made visible (player report: "not clear they are given or not").
static void TestBuddyOrders()
{
	using phantom::er::BuddyOrders::Hint;
	using phantom::er::BuddyOrders::OrderText;
	CHECK(OrderText(25, 4).starts_with("D-Walker: Search Mode"));
	CHECK(OrderText(21, 4).starts_with("D-Walker: Normal Mode"));
	CHECK(OrderText(20, 4).starts_with("D-Walker: Whistle"));
	CHECK(OrderText(9, 2) == "Buddy order 9");
	Hint h;
	using Say = Hint::Say;
	// A tap of LB: nothing.
	CHECK(h.Update(1000, true, true, 0, 0) == Say::kNothing);
	CHECK(h.Update(1200, true, false, 0, 0) == Say::kNothing);
	CHECK(h.Update(3000, true, false, 0, 0) == Say::kNothing);
	// A hold: the how-to once; pointed right, released without the click: "no order" after the answer window.
	CHECK(h.Update(10000, true, true, 0, 0) == Say::kNothing);
	CHECK(h.Update(10700, true, true, 0.9f, 0) == Say::kHowTo);
	CHECK(h.Update(11000, true, true, 0.9f, 0) == Say::kNothing);
	CHECK(h.Update(11500, true, false, 0, 0) == Say::kNothing);
	CHECK(h.Update(12000, true, false, 0, 0) == Say::kNothing);
	CHECK(h.Update(12400, true, false, 0, 0) == Say::kNoOrder);
	CHECK(h.Update(13000, true, false, 0, 0) == Say::kNothing);
	// The same, but the order comes (the click): no complaint.
	CHECK(h.Update(20000, true, true, 0.9f, 0) == Say::kNothing);
	CHECK(h.Update(20700, true, true, 0.9f, 0) == Say::kHowTo);
	CHECK(h.Update(21000, true, false, 0, 0) == Say::kNothing);
	CHECK(h.Update(21100, true, false, 0, 1) == Say::kNothing);
	CHECK(h.Update(23000, true, false, 0, 1) == Say::kNothing);
	// Pointed left/up/down only (Knock, Sonar: not the walker's): no complaint.
	CHECK(h.Update(30000, true, true, -0.9f, 1) == Say::kNothing);
	CHECK(h.Update(30700, true, true, -0.9f, 1) == Say::kHowTo);
	CHECK(h.Update(31000, true, false, 0, 1) == Say::kNothing);
	CHECK(h.Update(33000, true, false, 0, 1) == Say::kNothing);
	// Two orders given: no more how-to.
	CHECK(h.Update(40000, true, true, 0, 2) == Say::kNothing);
	CHECK(h.Update(41000, true, true, 0, 2) == Say::kNothing);
	CHECK(h.Update(41100, true, false, 0, 2) == Say::kNothing);
	// Not eligible (riding, a menu, no walker): nothing, and a hold in progress is forgotten.
	CHECK(h.Update(50000, true, true, 0.9f, 2) == Say::kNothing);
	CHECK(h.Update(50700, false, true, 0.9f, 2) == Say::kNothing);
	CHECK(h.Update(51000, true, false, 0, 2) == Say::kNothing);
	CHECK(h.Update(53000, true, false, 0, 2) == Say::kNothing);
}

// MenuWatch's groups (review): a HUD entry armed with a menu must not close it, nor keep it open.
static void TestMenuGroups()
{
	using phantom::er::MenuWatch::GroupOpen;
	std::vector<std::uint8_t> ui(0x46, 0);
	auto show = [&](int a_i, bool a_on) { ui[a_i] = a_on ? 0x03 : 0x01; };
	// The Start menu (0x25) with an item-pickup banner (0x30) that came up inside the latch.
	show(0x25, true);
	show(0x30, true);
	CHECK(GroupOpen({ 0x25, 0x30 }, {}, ui));
	show(0x30, false);  // the banner hides: the menu is still open
	CHECK(GroupOpen({ 0x25, 0x30 }, {}, ui));
	show(0x25, false);
	CHECK(!GroupOpen({ 0x25, 0x30 }, {}, ui));
	// The map (0x1c) with a boss bar (0x17, known as Elden Ring's HUD): closing the map closes the group.
	show(0x1c, true);
	show(0x17, true);
	CHECK(GroupOpen({ 0x1c, 0x17 }, { 0x17 }, ui));
	show(0x1c, false);
	CHECK(!GroupOpen({ 0x1c, 0x17 }, { 0x17 }, ui));
	// ... and a banner known as HUD hiding first doesn't close it.
	show(0x1c, true);
	show(0x17, false);
	CHECK(GroupOpen({ 0x1c, 0x17 }, { 0x17 }, ui));
	// Only HUD-too entries: the old rule (any hides -> closed).
	show(0x17, true);
	CHECK(GroupOpen({ 0x17, 0x1c }, { 0x17, 0x1c }, ui));
	show(0x1c, false);
	CHECK(!GroupOpen({ 0x17, 0x1c }, { 0x17, 0x1c }, ui));
	// Out of range or no table: hidden.
	CHECK(!GroupOpen({ 0x50 }, {}, ui));
	CHECK(!GroupOpen({ 0x25 }, {}, {}));

	// Cutscenes (review 2026-10-13): a skip press during one or its 500 ms tail never latches; one 1 s later does (it used
	// to be swallowed for 4 s: MGSV kept the pad inside a menu opened right after a cutscene).
	using phantom::er::MenuWatch::SwallowPress;
	using phantom::er::MenuWatch::WindowArm;
	CHECK(SwallowPress(true, 10000, 10000));
	CHECK(SwallowPress(false, 10400, 10000));
	CHECK(!SwallowPress(false, 11000, 10000));
	CHECK(!SwallowPress(false, 11000, 0));  // no cutscene seen yet
	// Margit's bar (0x12, 0x17) came up inside a late press's latch: never armed; the menu's own 0x25 is.
	CHECK(WindowArm({ 0x12, 0x17 }, { 0x12, 0x17 }).empty());
	CHECK((WindowArm({ 0x25, 0x17 }, { 0x12, 0x17 }) == std::vector<int>{ 0x25 }));
	CHECK((WindowArm({ 0x25 }, {}) == std::vector<int>{ 0x25 }));
	// A submenu replacing its menu (review): the map closing during Margit as the boss bar (0x12, HUD too) comes back arms
	// nothing; the inventory's own entries do; the main menu back from the inventory with the bar arms a mixed group.
	using phantom::er::MenuWatch::SubmenuGroup;
	CHECK(SubmenuGroup({ 0x12 }, { 0x12, 0x17 }, {}).empty());
	CHECK(SubmenuGroup({ 0x12, 0x17 }, { 0x12, 0x17 }, {}).empty());
	CHECK((SubmenuGroup({ 0x1f, 0x20 }, { 0x12 }, {}) == std::vector<int>{ 0x1f, 0x20 }));
	CHECK((SubmenuGroup({ 0x1c, 0x17, 0x12 }, { 0x12, 0x17 }, {}) == std::vector<int>{ 0x1c, 0x17, 0x12 }));
	CHECK(SubmenuGroup({ 0x1c }, {}, { 0x1c }).empty());  // after-cutscene window entries never arm
	CHECK(SubmenuGroup({}, {}, {}).empty());

	// Full-screen menus (live 2026-10-13: Start -> Inventory handed A/B/D-pad to MGSV): the HUD hiding inside an open menu.
	using phantom::er::MenuWatch::FullScreenMenu;
	CHECK(FullScreenMenu(false, true, true, false, false));    // main menu open, the HUD hides: the inventory
	CHECK(FullScreenMenu(true, false, false, false, false));   // its group closed meanwhile: still open while the HUD is hidden
	CHECK(!FullScreenMenu(true, false, false, true, false));   // the HUD is back: closed
	CHECK(!FullScreenMenu(false, false, true, false, false));  // the HUD hides with no menu (a grace, a dialog): not ours
	CHECK(!FullScreenMenu(false, true, false, false, false));  // already hidden when the menu opened (no transition)
	CHECK(!FullScreenMenu(true, true, false, false, true));    // a cutscene
}

#include "test_host_overlay.inc"
#include "test_host_pacing.inc"
#include "test_host_chord.inc"

// The real host link + console (er/Link.cpp, er/Console.cpp) against a live guest on a_where -- fake_mgsv.py
// or the real guest link -- driven by tools/phantomring/test_plugins.py. Prints one "PEER {json}" line.
static int RunPeer(const std::wstring& a_where, double a_seconds)
{
	using namespace phantom;
	auto& link = Link::Get();
	if (!link.Open(a_where)) {
		std::printf("PEER-FAIL can't open the mapping\n");
		return 1;
	}
	Frame          f{};
	std::uint64_t  states = 0, readyFrames = 0, poses = 0, hurts = 0, padWrites = 0, consoleAsks = 0;
	std::uint32_t  buddyFlags = 0, maxBones = 0, epochs = 0, lastEpoch = ~0u;
	std::uint16_t  buddyType = 0xFFFF, lifeMin = 0xFFFF;
	double         odoXZ = 0, lastOdo[3]{};
	bool           sawVehicle = false;
	std::map<std::uint32_t, std::uint64_t> events;
	static proto::PoseBlock pose;
	proto::PadState         pad{};
	const auto              t0 = Link::Qpc();
	auto                    elapsed = [&] { return Link::QpcToMs(Link::Qpc() - t0) / 1000.0; };
	double                  nextHurt = 1.0;
	while (elapsed() < a_seconds) {
		link.Tick();
		f.index++;
		f.guestAlive = link.GuestAlive();
		if (f.guestAlive && link.ReadGuestState(f.guest)) {
			++states;
			f.guestReady = (f.guest.flags & proto::kGuestReady) && !(f.guest.flags & proto::kGuestLoading);
			readyFrames += f.guestReady;
			buddyFlags |= f.guest.buddyFlags;
			buddyType = f.guest.buddyType;
			sawVehicle |= (f.guest.flags & proto::kGuestOnVehicle) != 0;
			lifeMin = std::min(lifeMin, f.guest.life);
			if (f.guest.odoEpoch != lastEpoch) {
				lastEpoch = f.guest.odoEpoch;
				++epochs;
			} else {
				odoXZ += std::hypot(f.guest.odo[0] - lastOdo[0], f.guest.odo[2] - lastOdo[2]);
			}
			std::copy(std::begin(f.guest.odo), std::end(f.guest.odo), lastOdo);
		}
		if (link.ReadPose(pose)) {
			++poses;
			maxBones = std::max(maxBones, pose.boneCount);
		}
		proto::GuestEvent e{};
		while (link.PopGuestEvent(e)) {
			++events[e.type];
		}
		// Left stick forward for the first half, then neutral; the packet bumps on every change.
		const std::int16_t ly = elapsed() < a_seconds / 2 ? 32767 : 0;
		if (pad.thumbLY != ly || !(pad.flags & proto::kPadConnected)) {
			pad.flags = proto::kPadConnected;
			pad.thumbLY = ly;
			++pad.packet;
			link.WritePad(pad);
			++padWrites;
		}
		if (f.guestAlive && elapsed() >= nextHurt) {
			nextHurt += 1.0;
			proto::HostEvent h{};
			h.type = proto::kHEvHurt;
			h.a = 10;
			h.b = proto::kHurtMelee;
			hurts += link.PushHostEvent(h);
		}
		auto* con = link.HostConsole();
		consoleAsks += Atomic(con->reqSeq).load(std::memory_order_acquire) != Atomic(con->respSeq).load(std::memory_order_acquire);
		Console::Poll(f);
		::Sleep(16);
	}
	std::string ev;
	for (const auto& [type, n] : events) {
		ev += std::format("{}\"{}\": {}", ev.empty() ? "" : ", ", type, n);
	}
	std::printf("PEER {\"attaches\": %u, \"states\": %llu, \"readyFrames\": %llu, \"epochs\": %u, \"odoXZ\": %.3f, \"buddyType\": %u, "
	            "\"buddyFlags\": %u, \"vehicle\": %s, \"lifeMin\": %u, \"poses\": %llu, \"maxBones\": %u, \"hurtsSent\": %llu, \"padWrites\": %llu, "
	            "\"consoleAsks\": %llu, \"events\": {%s}}\n",
		link.AttachCount(), static_cast<unsigned long long>(states), static_cast<unsigned long long>(readyFrames), epochs, odoXZ, buddyType, buddyFlags,
		sawVehicle ? "true" : "false", lifeMin, static_cast<unsigned long long>(poses), maxBones, static_cast<unsigned long long>(hurts),
		static_cast<unsigned long long>(padWrites), static_cast<unsigned long long>(consoleAsks), ev.c_str());
	std::fflush(stdout);
	return 0;
}

int main(int argc, char** argv)
{
	phantom::Log::Init(L"test_host.log", "test");
	if (argc >= 4 && std::string(argv[1]) == "--peer") {
		return RunPeer(phantom::Widen(argv[2]), std::atof(argv[3]));
	}
	TestExpr();
	TestAnchor();
	TestKinematic();
	TestLinkAndConsole();
	TestOverlay();
	TestInteraction();
	TestRevival();
	TestMenuGroups();
	TestBuddyOrders();
	TestPacing();
	TestChord();
	TestFallWatch();
	TestLadder();
	TestSupportedStart();
	if (argc >= 2) {
		TestShippedIni(phantom::Widen(argv[1]).c_str());
	}
	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
