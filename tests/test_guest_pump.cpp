// Offline integration test of the guest's Lua pump (mgsv/Pump.cpp + Lua.cpp + the embedded script)
// against a stock Lua 5.1.5 VM with a mock of the MGSV globals: script loading and registry refs,
// the per-update Tick feeding the frame state, the whole boot sequence (title -> Continue -> another
// mission -> load the sandbox -> setup -> D-Walker -> centre -> ready), the dev console over a real
// mapping, and the phantomring.lua override. Built only when PHANTOM_LUA51_SRC points at Lua 5.1.5's
// src/ (target check_guest).
#include "ActorProxy.h"
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
TppBuddy2BlockController = {
	GetActiveBuddyType = function() return MOCK.blockLoaded end,
	IsLoading = function() return MOCK.loading end,
	SetVarsBuddyType = function(t) rec('setvars ' .. t) end,
	Load = function() rec('load'); MOCK.blockLoaded = 4 end,
	CallBuddy = function(t, p, yaw) rec('call ' .. t); MOCK.buddy = 0x2001 end }
GameObject = {
	GetGameObjectIdByIndex = function(cls, i)
		if cls == 'TppCommonWalkerGear2' and i == 0 and MOCK.buddy then return MOCK.buddy end
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

// Pumps "the game" for a_ms: one camera frame + one outermost pcall return every 10 ms.
static void Run(int a_ms)
{
	static std::uint64_t frame = 0;
	for (int t = 0; t < a_ms; t += 10) {
		state::SetFrame({ true, ++frame, ::GetTickCount64(), { 10.0f, 0.5f, -4.0f }, 0.25f, {} });
		const int top = lua_gettop(g_L);
		pump::Run(g_L);
		CHECK(lua_gettop(g_L) == top);  // the pump leaves the engine's stack exactly as it found it
		::Sleep(10);
	}
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
	CHECK(state::Lua().valid && ::GetTickCount64() - state::Lua().tickMs < 500);  // Tick (kept by ref) still runs

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
