// Offline integration test of the guest's Lua pump (mgsv/Pump.cpp + Lua.cpp + the embedded script)
// against a stock Lua 5.1.5 VM with a mock of the MGSV globals: script loading and registry refs,
// the per-update Tick feeding the frame state, the whole boot sequence (title -> Continue -> another
// mission -> load the sandbox -> setup -> D-Walker -> centre -> ready), the dev console over a real
// mapping, the phantomring.lua override, and the buddy dismissed from the iDroid against one MGSV lost
// (AdoptBuddy / ReviveBuddy, docs/GUEST.md 8.3). Built only when PHANTOM_LUA51_SRC points at Lua 5.1.5's
// src/ (target check_guest).
#include "ActorProxy.h"
#include "Quiet.h"
#include "FultonAnchor.h"
#include "Menu.h"
#include "Config.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Lua.h"
#include "Mapping.h"
#include "Pump.h"
#include "State.h"

extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace phantom;
using namespace phantom::guest;

static int g_failures = 0;
#define CHECK(x)                                                    \
	do {                                                            \
		if (!(x)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			++g_failures;                                           \
		}                                                           \
	} while (0)

// The engine side: just enough of MGSV's Lua world for the boot sequence. MOCK.* is what the test turns.
static const char kMock[] = R"(
MOCK = { title = true, prepared = true, mission = 5, log = {}, blockLoaded = nil, loading = false, buddy = nil }
local function rec(s) MOCK.log[#MOCK.log + 1] = s end
vars = setmetatable({ playerPosX = 1, playerPosY = 2, playerPosZ = 3, playerVehicleGameObjectId = 65535 },
	{ __index = function(t, k) if k == 'missionCode' then return MOCK.mission end end })
gvars = setmetatable({}, { __index = function(t, k) if k == 'ini_isTitleMode' then return MOCK.title end end })
mvars = {}
PlayerStatus = { STAND = 4, NORMAL_ACTION = 7 }
PlayerInfo = { AndCheckStatus = function(t) return t[1] == 4 or t[1] == 7 end }
TppMain = {}
TppSequence = { GetCurrentSequenceName = function() return MOCK.title and 'Seq_Demo_Title' or 'Seq_Game_MainGame' end,
	IsMissionPrepareFinished = function() return MOCK.prepared end }
TppMission = { IsHelicopterSpace = function() return false end,
	ExecuteMissionFinalize = function() rec('finalize ' .. tostring(gvars.mis_nextMissionCodeForMissionClear)) end,
	ReserveMissionClear = function(t) rec('reserve ' .. t.nextMissionId) end }
TppDefine = { MISSION_CLEAR_TYPE = { FROM_HELISPACE = 7 }, WEATHER = { SUNNY = 0 } }
DemoDaemon = { IsDemoPlaying = function() return false end }
TppUiCommand = { IsEndLoadingTips = function() return true end }
BuddyType = { NONE = 0, HORSE = 1, DOG = 2, QUIET = 3, WALKER_GEAR = 4 }
TppBuddyService = { DidObtainBuddyType = function() return true end, CanSortieBuddyType = function() return true end,
	IsDeadBuddyType = function() return false end }
-- MOCK.blockLoaded: the buddy block MGSV has (BuddyType, 0 = NONE); MOCK.buddy: its object (nil = none, or gone).
local BUDDY_CLASS = { [1] = 'TppHorse2', [2] = 'TppBuddyDog2', [3] = 'TppBuddyQuiet2', [4] = 'TppCommonWalkerGear2' }
TppBuddy2BlockController = {
	GetActiveBuddyType = function() return MOCK.blockLoaded end,
	IsLoading = function() return MOCK.loading end,
	SetVarsBuddyType = function(t) rec('setvars ' .. t); MOCK.varsType = t end,
	Load = function() rec('load'); MOCK.blockLoaded = MOCK.varsType; MOCK.buddy = nil end,
	CallBuddy = function(t, p, yaw) rec('call ' .. t); MOCK.buddy = 0x2001 end }
GameObject = {
	GetGameObjectIdByIndex = function(cls, i)
		if i == 0 and MOCK.buddy and cls == BUDDY_CLASS[MOCK.blockLoaded] then return MOCK.buddy end
		return 65535 end,
	SendCommand = function(target, cmd)
		if type(target) == 'table' then rec(cmd.id) return end
		if cmd.id == 'GetPosition' and target == MOCK.buddy then return { 7, 0, -2 } end
	end }
Player = {}
TppGameStatus = { Set = function(a, b) rec('status ' .. b) end }
TppClock = { SetTime = function(t) rec('time ' .. t) end }
TppCommand = { Weather = { SetClockTimeScale = function() end } }
TppWeather = { ForceRequestWeather = function() end }
)";

static lua_State* g_L = nullptr;

// The save guards (mgsv/WindowHidden.cpp, SaveGuard.cpp, not linked here): incomplete in this test, so the boot must leave MGSV's
// Auto-Aim Correction as it is (review: it was gated on [Saves] Block alone).
namespace phantom::guest::hidden
{
	bool SavesBlocked() { return false; }
}
namespace phantom::guest::saves
{
	bool SteamGuarded() { return true; }
}
// ActorProxy's game glue (mgsv/ActorProxyGame.cpp, not linked here; [Proxies] is off in this test).
namespace phantom::guest::proxy
{
	void SetPool(const plan::Pool&) {}
	bool AutoAimWanted(bool) { return false; }
	bool AllPlaced() { return false; }
	bool Active() { return false; }
	bool TakePoolRequest() { return false; }
	void LuaStep(std::uint64_t, const std::function<std::string(const std::string&)>&) {}
	std::string Fulton(std::uint64_t, bool, const char*) { return {}; }
}
// The Fulton anchor (mgsv/FultonAnchor.cpp) and MGSV's menus (mgsv/Menu.cpp, read by Pad.cpp's iDroid request): not linked here.
namespace phantom::guest::fulton
{
	void LuaStep(std::uint64_t, const std::function<std::string(const std::string&)>&) {}
	void Reset(const char*) {}
}
namespace phantom::guest::menu
{
	bool Open() { return false; }
}
// The suppressor (mgsv/Suppressor.cpp, not linked here): its setters unverified, so the boot leaves [Loadout] Suppressors alone.
namespace phantom::guest::suppressor
{
	bool        SettersAvailable() { return false; }
	std::string ApplyLoadout(const std::string&, unsigned) { return "not in this test"; }
}
// Quiet (mgsv/Quiet.cpp, not linked here; [Quiet] is off in this test).
namespace phantom::guest::quiet
{
	void        LuaStep(std::uint64_t, bool, bool, const LuaCall&) {}
	std::string BeforeDeploy(bool, const LuaCall&) { return {}; }
	void        AtReady(bool, const LuaCall&) {}
	bool        KeepMarkers() { return false; }
}

static bool MockLog(const char* a_entry)
{
	lua_getglobal(g_L, "MOCK");
	lua_getfield(g_L, -1, "log");
	const int n = static_cast<int>(lua_objlen(g_L, -1));
	bool      found = false;
	for (int i = 1; i <= n && !found; ++i) {
		lua_rawgeti(g_L, -1, i);
		found = std::strcmp(lua_tostring(g_L, -1), a_entry) == 0;
		lua_pop(g_L, 1);
	}
	lua_pop(g_L, 2);
	return found;
}

static void SetMock(const char* a_assignment)
{
	const std::string code = std::string("MOCK.") + a_assignment;
	if (luaL_dostring(g_L, code.c_str()) != 0) {
		std::printf("mock: %s\n", lua_tostring(g_L, -1));
	}
}

static void MockDo(const char* a_code)
{
	if (luaL_dostring(g_L, a_code) != 0) {
		std::printf("mock: %s\n", lua_tostring(g_L, -1));
		lua_pop(g_L, 1);
		++g_failures;
	}
}

// Lines of the pump's log (test_guest_pump.log, flushed line by line) that hold a_text.
static int LogCount(const char* a_text)
{
	FILE* f = std::fopen("test_guest_pump.log", "rb");
	if (!f) {
		return -1;
	}
	int  n = 0;
	char line[2048];
	while (std::fgets(line, sizeof(line), f)) {
		n += std::strstr(line, a_text) != nullptr;
	}
	std::fclose(f);
	return n;
}

// Pumps "the game" for a_ms: one camera frame + one outermost pcall return every 10 ms. The frames carry the pump's clock
// (pump::NowForTests: real time plus every Skip).
static void Run(int a_ms)
{
	static std::uint64_t frame = 0;
	for (int t = 0; t < a_ms; t += 10) {
		state::SetFrame({ true, ++frame, pump::NowForTests(), { 10.0f, 0.5f, -4.0f }, 0.25f, {} });
		const int top = lua_gettop(g_L);
		pump::Run(g_L);
		CHECK(lua_gettop(g_L) == top);  // the pump leaves the engine's stack exactly as it found it
		::Sleep(10);
	}
}

// The pump's clock jumps a_ms ahead, then one boot step runs at the new time (the boot steps every 250 ms of it). The Lua side
// still counts real frames: Run for what it re-reads (a buddy's object id every 60 ticks).
static void Skip(std::uint64_t a_ms)
{
	pump::SkipClockForTests(a_ms);
	Run(50);
}

template <class F>
static bool Until(F&& a_cond, int a_ms)
{
	for (int t = 0; t < a_ms; t += 50) {
		if (a_cond()) {
			return true;
		}
		Run(50);
	}
	return a_cond();
}

// ---- the buddy dismissed from the iDroid, or lost (docs/GUEST.md 8.3; Pump.cpp AdoptBuddy / ReviveBuddy, BuddyLogic.h) ----

// An MGSV menu (the iDroid) open for a few boot steps, then closed: AdoptBuddy's "menu last seen".
static void MenuSeen()
{
	state::SetMenuOpen(true);
	Run(300);
	state::SetMenuOpen(false);
}

// MGSV has a_type's buddy out (the iDroid's "Request deployment", or a call answered); AdoptBuddy takes it once it has been out 2 s.
static void BuddyOut(int a_type)
{
	char code[80];
	std::snprintf(code, sizeof(code), "MOCK.blockLoaded = %d MOCK.buddy = 0x2001", a_type);
	MockDo(code);
	Run(300);
	Skip(2500);
}

// MGSV's active buddy type goes NONE (the object with it); AdoptBuddy judges it once that has lasted 2 s.
static void GoesNone()
{
	MockDo("MOCK.blockLoaded = 0 MOCK.buddy = nil");
	Skip(250);   // NONE's start, as AdoptBuddy sees it
	Skip(2500);  // judged
}

// The pump's wiring of 0.9.0's dismissal (f4edaa8): MGSV's leave order with its menu open (state::NoteBuddyLeave, noted by the buddies'
// handler detours) makes a NONE the player's dismissal however long the buddy takes to go, and keeps ReviveBuddy off it meanwhile;
// without one, a NONE that began within the buddy's window after the menu (D-Walker / D-Horse 20 s, Quiet / D-Dog 30 s) is; any
// other NONE is a buddy MGSV lost, and is called back. BuddyLogic's own cases: test_guest_logic's TestBuddyDismiss.
static void TestBuddyDismissal()
{
	auto& c = MutableCfg();
	CHECK(c.buddy == "walker" && c.buddyDismiss && c.buddyRevive);
	// Every call of PhantomRing.Buddy outside a deployment step is ReviveBuddy's: recorded as "Buddy <kind>".
	MockDo("MOCK.realBuddy = PhantomRing.Buddy "
	       "PhantomRing.Buddy = function(kind, ...) MOCK.log[#MOCK.log + 1] = 'Buddy ' .. kind return MOCK.realBuddy(kind, ...) end");
	Skip(2500);
	Skip(2500);  // the deployed walker is the guest's own by now (AdoptBuddy)
	CHECK(state::Lua().buddyActive == state::kBuddyTypeWalker && state::Lua().buddyId == 0x2001);

	// MGSV loses the walker by itself: no leave order, no MGSV menu seen since the boot. AdoptBuddy keeps it ("calling it back"),
	// ReviveBuddy calls it back once it has been gone 10 s.
	{
		const char* said = "(D-Walker expected, no iDroid DISMISS seen) -- calling it back";
		const int   before = LogCount(said);
		SetMock("log = {}");
		GoesNone();
		CHECK(c.buddy == "walker");
		CHECK(LogCount(said) == before + 1);
		CHECK(!MockLog("Buddy walker"));  // gone 2.5 s
		Skip(9000);
		CHECK(MockLog("Buddy walker"));   // gone 11.5 s: PhantomRing.Buddy('walker', ..., true)
		// ... and the script brings it back: its block (once its last load is 600 ticks old), then the call.
		CHECK(Until([] { return MockLog("call 4"); }, 15000));
		CHECK(Until([] { return state::Lua().buddyActive == state::kBuddyTypeWalker && state::Lua().buddyId == 0x2001; }, 3000));
	}

	// The iDroid's DISMISS D-WALKER: MGSV orders it to leave while its menu is open, and it takes longer to go than its menu window
	// (20 s) allows. ReviveBuddy leaves it alone; its NONE, 25 s after the menu, is the player's dismissal by the order.
	{
		const char* ordered = "with its menu open (the iDroid";
		const char* held = "s -- not called back: MGSV ordered it to leave from the iDroid";
		const char* dismissed = "the player dismissed D-Walker from the iDroid (MGSV's leave order, NONE";
		const int   ordered0 = LogCount(ordered), held0 = LogCount(held), dismissed0 = LogCount(dismissed);
		SetMock("log = {}");
		state::SetMenuOpen(true);
		Run(300);
		state::NoteBuddyLeave(state::kBuddyTypeWalker, pump::NowForTests());
		Run(300);
		state::SetMenuOpen(false);
		CHECK(LogCount(ordered) == ordered0 + 1);
		MockDo("MOCK.buddy = nil");  // fultoned out: its object goes, MGSV keeps WALKER_GEAR active meanwhile
		Run(1000);                   // the script re-reads the buddy's id every 60 ticks
		CHECK(state::Lua().buddyActive == state::kBuddyTypeWalker && state::Lua().buddyId == 0xFFFF);
		Skip(12000);
		Skip(12000);
		CHECK(!MockLog("Buddy walker"));  // gone 25 s: without the order it was called back at 10 s
		CHECK(LogCount(held) == held0 + 1);
		MockDo("MOCK.blockLoaded = 0");
		Skip(250);
		Skip(2500);
		CHECK(c.buddy == "none");
		CHECK(LogCount(dismissed) == dismissed0 + 1);
		CHECK(link::CurrentStatusText() == "MGSV: ready (Mother Base)");
		Skip(15000);
		CHECK(!MockLog("Buddy walker") && !MockLog("load") && !MockLog("call 4"));  // never called back
	}

	// No leave order seen (D-Horse's handler isn't hooked, a hook missing): the menu window, counted from NONE's start. D-Walker NONE
	// 18.75-19.3 s after its menu, inside its 20 s, is the player's dismissal, though judged 2.5 s later (0.9.0 counted to the
	// decision).
	{
		BuddyOut(state::kBuddyTypeWalker);  // the player deploys D-Walker again from the iDroid
		CHECK(c.buddy == "walker");
		const char* dismissed = "the player dismissed D-Walker from the iDroid (NONE";
		const int   before = LogCount(dismissed);
		SetMock("log = {}");
		MenuSeen();
		Skip(18500);
		GoesNone();
		CHECK(c.buddy == "none");
		CHECK(LogCount(dismissed) == before + 1);  // "(NONE 18.9 s after its menu, no leave order seen)"
		CHECK(!MockLog("Buddy walker"));
	}

	// ... past its window: D-Walker NONE 21 s after the menu, no leave order -- lost, called back.
	{
		BuddyOut(state::kBuddyTypeWalker);
		CHECK(c.buddy == "walker");
		const char* said = "(D-Walker expected, no iDroid DISMISS seen: no leave order, its menu last open";
		const int   before = LogCount(said);
		SetMock("log = {}");
		MenuSeen();
		Skip(21000);
		GoesNone();
		CHECK(c.buddy == "walker");
		CHECK(LogCount(said) == before + 1);
		Skip(9000);
		CHECK(MockLog("Buddy walker"));
		BuddyOut(state::kBuddyTypeWalker);  // back
		CHECK(c.buddy == "walker");
	}

	// Quiet's window is 30 s (her far post; D-Dog's too): NONE 25 s after the menu, past D-Walker's 20 s, no leave order -- dismissed.
	{
		BuddyOut(state::kBuddyTypeQuiet);  // the player swaps D-Walker for Quiet from the iDroid
		CHECK(c.buddy == "quiet");
		const char* dismissed = "the player dismissed Quiet from the iDroid (NONE";
		const int   before = LogCount(dismissed);
		SetMock("log = {}");
		MenuSeen();
		Skip(25000);
		GoesNone();
		CHECK(c.buddy == "none");
		CHECK(LogCount(dismissed) == before + 1);
		CHECK(!MockLog("Buddy quiet"));
	}

	// ... and D-Dog NONE 31 s after the menu, no leave order: past its 30 s -- lost, called back.
	{
		BuddyOut(state::kBuddyTypeDog);
		CHECK(c.buddy == "dog");
		const char* said = "(D-Dog expected, no iDroid DISMISS seen: no leave order, its menu last open";
		const int   before = LogCount(said);
		SetMock("log = {}");
		MenuSeen();
		Skip(31000);
		GoesNone();
		CHECK(c.buddy == "dog");
		CHECK(LogCount(said) == before + 1);
		Skip(9000);
		CHECK(MockLog("Buddy dog"));
		BuddyOut(state::kBuddyTypeDog);  // back
	}
	MockDo("PhantomRing.Buddy = MOCK.realBuddy");
}

int main()
{
	Log::Init(L"test_guest_pump.log", "test");
	Log::SetDebug(true);
	MutableCfg() = Config{};
	// This test plays the Mother Base boot (the shipped default is Africa at the airport): pin it.
	MutableCfg().mission = 30050;
	MutableCfg().hasPosition = false;
	MutableCfg().rotYDeg = 0.0f;
	MutableCfg().luaOverride = L"test_guest_pump_override.lua";
	::DeleteFileW((ModuleDir() + L"test_guest_pump_override.lua").c_str());

	g_L = luaL_newstate();
	luaL_openlibs(g_L);
	CHECK(luaL_dostring(g_L, kMock) == 0);
	CHECK(luaL_dostring(g_L, "vars.optionSelectedIndices = { [11] = 0 }") == 0);  // Auto-Aim Correction ON
	lua::UseApiForTests({ reinterpret_cast<void*>(&lua_pcall), reinterpret_cast<void*>(&luaL_loadbuffer), reinterpret_cast<void*>(&lua_gettop),
		reinterpret_cast<void*>(&lua_settop), reinterpret_cast<void*>(&lua_tolstring), reinterpret_cast<void*>(&lua_type),
		reinterpret_cast<void*>(&lua_tonumber), reinterpret_cast<void*>(&lua_toboolean), reinterpret_cast<void*>(&lua_pushvalue),
		reinterpret_cast<void*>(&lua_pushstring), reinterpret_cast<void*>(&lua_rawgeti), reinterpret_cast<void*>(&luaL_ref),
		reinterpret_cast<void*>(&luaL_unref) });

	// A host on a real mapping, for the console and the status text.
	wchar_t temp[MAX_PATH];
	::GetTempPathW(MAX_PATH, temp);
	const std::wstring path = std::wstring(L"file:") + temp + L"phantomring_guest_pump_test.bin";
	Mapping            host;
	CHECK(host.Open(path, proto::kMappingBytes, true));
	auto* hdr = reinterpret_cast<proto::Header*>(host.Base());
	hdr->version = proto::kVersion;
	hdr->sessionNonce = 0x77;
	Atomic(hdr->magic).store(proto::kMagic);
	link::Start(path);

	// Title: the script loads, Tick reports the title, the boot sequence presses Continue.
	Run(600);
	CHECK(lua::Available());
	lua_getglobal(g_L, "PhantomRing");
	CHECK(lua_istable(g_L, -1));
	lua_pop(g_L, 1);
	// What a save would keep in the player's MGSV save (review 2026-10-31): the Phantom Cigar in an item slot, the box and its
	// poster. Recorded here; with the guards incomplete none of them may be called.
	CHECK(luaL_dostring(g_L, "PhantomRing.CigarItem = function() MOCK.log[#MOCK.log + 1] = 'cigar item' return 'put' end "
	                         "PhantomRing.Poster = function() MOCK.log[#MOCK.log + 1] = 'poster' return 'done' end "
	                         "PhantomRing.BoxItem = function() MOCK.log[#MOCK.log + 1] = 'box item' return 'ok' end") == 0);
	CHECK(state::Lua().valid && (state::Lua().flags & state::kLTitle));
	CHECK(state::CurrentBoot() == state::Boot::kTitle);
	CHECK(link::CurrentStatusText().find("title") != std::string::npos);

	// Continue loads a story mission (10040): the guest asks for the sandbox (direct load first).
	SetMock("title = false");
	SetMock("mission = 10040");
	CHECK(Until([] { return MockLog("finalize 30050"); }, 3000));
	CHECK(state::CurrentBoot() == state::Boot::kLoadSandbox);
	CHECK(link::CurrentStatusText().find("Mother Base") != std::string::npos);

	// The sandbox arrives: settle, setup (soldiers, invincible, clock), D-Walker, centre, ready.
	SetMock("mission = 30050");
	CHECK(Until([] { return state::CurrentBoot() == state::Boot::kReady; }, 15000));
	CHECK(MockLog("status S_DISABLE_PLAYER_DAMAGE"));
	CHECK(MockLog("time 12:00:00"));
	CHECK(MockLog("setvars 4") && MockLog("load") && MockLog("call 4"));
	const auto centre = state::Centre();
	CHECK(centre && centre->x == 10.0f && centre->z == -4.0f);
	CHECK(link::CurrentStatusText() == "MGSV: ready (Mother Base, D-Walker deployed)");
	// [Boot] AutoAim = false with the save guards incomplete: left as it is (it could be saved).
	CHECK(luaL_dostring(g_L, "return vars.optionSelectedIndices[11]") == 0 && lua_tonumber(g_L, -1) == 0);
	lua_pop(g_L, 1);
	// ... and so are the Phantom Cigar's item slot, the box and its poster (review 2026-10-31)
	CHECK(!MockLog("cigar item") && !MockLog("poster") && !MockLog("box item"));
	const auto lua = state::Lua();
	CHECK(lua.InGame() && lua.mission == 30050 && lua.buddyActive == 4 && lua.buddyId == 0x2001);
	CHECK(lua.buddyPosValid && lua.buddyPos.x == 7.0f && lua.buddyPos.z == -2.0f);

	// Frame -> pump commands run inside the next Tick (the treadmill's Lua warp).
	state::QueueCommand("warp 1 2 3 0.5");
	Run(100);
	CHECK(MockLog("Warp"));

	// Dev console: Lua through PhantomRing.Console, errors reported with status 1.
	auto* console = reinterpret_cast<proto::Console*>(host.Base() + proto::kOffGuestConsole);
	CHECK(Until([] { return link::Attached(); }, 3000));
	std::strcpy(console->request, "return 1 + 1, vars.missionCode, nil, PhantomRing ~= nil");
	Atomic(console->reqSeq).store(console->reqSeq + 1);
	CHECK(Until([&] { return console->respSeq == console->reqSeq; }, 2000));
	CHECK(std::string(console->response) == "2\t30050\tnil\ttrue");
	CHECK(console->status == 0);
	std::strcpy(console->request, "error('nope')");
	Atomic(console->reqSeq).store(console->reqSeq + 1);
	CHECK(Until([&] { return console->respSeq == console->reqSeq; }, 2000));
	CHECK(console->status == 1 && std::string(console->response).find("nope") != std::string::npos);
	std::strcpy(console->request, "this is not lua");
	Atomic(console->reqSeq).store(console->reqSeq + 1);
	CHECK(Until([&] { return console->respSeq == console->reqSeq; }, 2000));
	CHECK(console->status == 1);

	TestBuddyDismissal();

	// Live iteration: phantomring.lua next to the DLL replaces the script (picked up within ~1 s).
	{
		FILE* f = _wfopen((ModuleDir() + L"test_guest_pump_override.lua").c_str(), L"wb");
		std::fputs("PhantomRing = PhantomRing or {}\nPhantomRing.overridden = 42\nreturn 1\n", f);
		std::fclose(f);
	}
	Run(1500);
	lua_getglobal(g_L, "PhantomRing");
	lua_getfield(g_L, -1, "overridden");
	CHECK(lua_tonumber(g_L, -1) == 42);
	lua_pop(g_L, 2);
	CHECK(state::Lua().valid && pump::NowForTests() - state::Lua().tickMs < 500);  // Tick (kept by ref) still runs

	// Leaving the sandbox (e.g. MGSV continued from a checkpoint elsewhere) sends it back.
	SetMock("log = {}");
	SetMock("mission = 30010");
	CHECK(Until([] { return MockLog("finalize 30050"); }, 3000));

	::DeleteFileW((ModuleDir() + L"test_guest_pump_override.lua").c_str());
	if (g_failures) {
		std::printf("test_guest_pump: %d FAILED (see test_guest_pump.log)\n", g_failures);
		std::fflush(stdout);
		::ExitProcess(1);
	}
	std::printf("test_guest_pump: all passed\n");
	std::fflush(stdout);
	::ExitProcess(0);  // the link worker never exits
}
