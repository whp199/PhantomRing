-- Offline test of the guest's embedded Lua (mgsv/GuestScript.cpp) under stock Lua 5.1 with a mock of
-- the MGSV globals it touches. Checks syntax, the Tick result order Pump.cpp relies on, the command
-- parser, the buddy deployment state machine, mission loading and the sandbox setup -- and that a
-- missing or throwing engine API never breaks it.
--
--   lua5.1 tests/test_guest_script.lua mgsv/GuestScript.cpp
local path = assert(arg[1], 'usage: lua5.1 test_guest_script.lua mgsv/GuestScript.cpp')
local src = assert(io.open(path)):read('*a')
local script = assert(src:match('R"PRLUA%((.-)%)PRLUA"'), 'embedded script not found')

local failures = 0
local function check(cond, what)
	if not cond then
		failures = failures + 1
		print('FAIL ' .. what)
	end
end

-- ---- mock engine ----------------------------------------------------------------------------------
local log = {}
local function record(...) log[#log + 1] = table.concat({ ... }, ' ') end

local status = { STAND = 1, SQUAT = 2, CRAWL = 3, NORMAL_ACTION = 4, DASH = 5, ON_VEHICLE = 6, CQC_HOLD = 7, DEAD = 8, GUN_FIRE = 9 }
local active = { [1] = true, [4] = true }

local function vec(x, y, z)
	return setmetatable({}, { __index = { GetX = function() return x end, GetY = function() return y end, GetZ = function() return z end } })
end

local function freshEnv()
	local blockLoaded, blockLoading, buddyObject, called = nil, false, nil, 0
	local env = {
		vars = { missionCode = 30050, playerPosX = 1, playerPosY = 2, playerPosZ = 3, playerLife = 6000, playerLifeMax = 6000, playerVehicleGameObjectId = 65535 },
		gvars = { ini_isTitleMode = false },
		mvars = {},
		PlayerStatus = status,
		PlayerInfo = { AndCheckStatus = function(t) return active[t[1]] == true end },
		TppSequence = { GetCurrentSequenceName = function() return 'Seq_Game_MainGame' end, IsMissionPrepareFinished = function() return true end },
		TppMission = {
			IsHelicopterSpace = function(c) return c == 40050 end,
			ReserveMissionClear = function(t) record('reserve', t.missionClearType, t.nextMissionId) end,
			ExecuteMissionFinalize = function() record('finalize') end,
		},
		TppDefine = { MISSION_CLEAR_TYPE = { FROM_HELISPACE = 7 }, WEATHER = { SUNNY = 0 } },
		DemoDaemon = { IsDemoPlaying = function() return false end },
		TppUiCommand = { IsMbDvcTerminalOpened = function() return false end },  -- IsShowPopup missing on purpose
		BuddyType = { NONE = 0, HORSE = 1, DOG = 2, QUIET = 3, WALKER_GEAR = 4 },
		TppBuddyService = {
			DidObtainBuddyType = function(t) return t ~= 3 end,
			CanSortieBuddyType = function() return true end,
			IsDeadBuddyType = function() return false end,
			SetSortieBuddyType = function(t) record('sortie', t) end,
		},
		TppBuddy2BlockController = {
			GetActiveBuddyType = function() return blockLoaded end,
			IsLoading = function() return blockLoading end,
			SetVarsBuddyType = function(t) record('setvars', t) end,
			Load = function() blockLoading = true; record('load') end,
			CallBuddy = function(t, pos, yaw, flag)
				called = called + 1
				record('call', t, pos and pos:GetX(), yaw, tostring(flag))
				buddyObject = 0x2001
			end,
		},
		GameObject = {
			GetGameObjectIdByIndex = function(cls, i)
				if cls == 'TppCommonWalkerGear2' and i == 0 and buddyObject then return buddyObject end
				if cls == 'TppSoldier2' and i < 5 then return 0x100 + i end
				return 65535
			end,
			SendCommand = function(target, cmd)
				if type(target) == 'table' then
					record('send', target.type, cmd.id, cmd.pos and table.concat(cmd.pos, ',') or '', tostring(cmd.rotY))
					return
				end
				if cmd.id == 'GetPosition' and target == buddyObject then return vec(10, 0, -4) end
				record('send', target, cmd.id, tostring(cmd.enabled), cmd.pos and table.concat(cmd.pos, ',') or '')
			end,
		},
		Player = {
			GetGameObjectIdIsRiddenToLocal = function() return 65535 end,
			RequestToSetCameraNoise = function(t) record('noise', t.levelX, t.levelY, t.time, t.decayRate) end,
			SetBloodToModel = function(t) record('blood', t.pos:GetX(), t.pos:GetY(), t.pos:GetZ(), t.radius) end,
		},
		TppGameStatus = { Set = function(a, b) record('status', a, b) end },
		TppClock = { SetTime = function(t) record('time', t) end },
		TppCommand = { Weather = { SetClockTimeScale = function(s) record('scale', s) end } },
		TppWeather = { ForceRequestWeather = function(w, t) record('weather', w, t) end },
		Vector3 = vec,
		-- stock libs
		math = math, string = string, table = table, pairs = pairs, ipairs = ipairs, type = type, tostring = tostring,
		tonumber = tonumber, pcall = pcall, error = error, select = select, setmetatable = setmetatable,
	}
	env._G = env
	env._finishLoad = function() blockLoading = false; blockLoaded = 4 end
	return env
end

local function load(env)
	local chunk = assert(loadstring(script, '=GuestScript'))
	setfenv(chunk, env)
	local version = chunk()
	check(version == 1, 'script returns its version')
	check(type(env.PhantomRing) == 'table', 'defines PhantomRing')
	return env.PhantomRing
end

-- ---- syntax + Tick result order --------------------------------------------------------------------
local env = freshEnv()
local PR = load(env)
local r = { PR.Tick('') }
local F = PR.F
check(r[1] == F.PREPARED + F.SEQ_GAME + F.STAND + F.NORMAL, 'flags: prepared, game sequence, stand, normal (got ' .. tostring(r[1]) .. ')')
check(r[2] == 30050, 'mission code')
check(r[3] == 'Seq_Game_MainGame', 'sequence name')
check(r[4] == nil and r[5] == nil and r[6] == nil, 'no buddy yet')
check(r[10] == nil, 'riding nothing')
check(r[11] == '', 'no command results')

-- ---- commands ----------------------------------------------------------------------------------------
log = {}
r = { PR.Tick('warp 1.5 2 -3 0.25;owarp 4097 1 2 3 0.5;bogus;warpwait 0 0 0 0') }
check(r[11]:find('warp=ok') ~= nil, 'warp command ran: ' .. r[11])
check(r[11]:find('owarp=ok') ~= nil, 'owarp command ran')
check(r[11]:find('bogus=unknown') ~= nil, 'unknown command reported')
check(log[1] == 'send TppPlayer2 Warp 1.5,2,-3 0.25', 'treadmill uses Warp: ' .. tostring(log[1]))
check(log[2] == 'send 4097 SetPosition nil 1,2,3', 'object SetPosition shape: ' .. tostring(log[2]))
check(log[#log] == 'send TppPlayer2 WarpAndWaitBlock 0,0,0 0', 'warpwait uses WarpAndWaitBlock: ' .. tostring(log[#log]))

-- ---- hurt feedback (Elden Ring hit on Snake) -------------------------------------------------------
log = {}
r = { PR.Tick('hurt 0.5 0.75 0.1 1 2.2 3 0.25;hurt 0.2 0.3 0.1') }
check(r[11]:find('hurt=noise true blood true') ~= nil, 'hurt runs noise + blood: ' .. r[11])
check(log[1] == 'noise 0.5 0.5 0.75 0.1', 'camera noise keys: ' .. tostring(log[1]))
check(log[2] == 'blood 1 2.2 3 0.25', 'blood at a Vector3 with radius: ' .. tostring(log[2]))
check(log[3] == 'noise 0.2 0.2 0.3 0.1' and log[4] == nil, 'hurt without a blood point only shakes: ' .. tostring(log[3]))

-- ---- buddy deployment ------------------------------------------------------------------------------
log = {}
check(PR.Buddy('walker', 5, 0, 5, 1.0):find('^block requested') ~= nil, 'buddy: requests the D-Walker block')
check(log[1] == 'sortie 4' and log[2] == 'setvars 4' and log[3] == 'load', 'buddy: sortie + SetVarsBuddyType + Load')
check(PR.Buddy('walker', 5, 0, 5, 1.0) == 'loading block', 'buddy: waits while loading')
env._finishLoad()
local b = PR.Buddy('walker', 5, 0, 5, 1.0)
check(b:find('^called true') ~= nil, 'buddy: CallBuddy once the block is active: ' .. b)
check(log[#log] == 'call 4 5 1 true', 'CallBuddy(type, Vector3, yaw, true): ' .. tostring(log[#log]))
b = PR.Buddy('walker', 5, 0, 5, 1.0)
check(b:find('^active TppCommonWalkerGear2') ~= nil, 'buddy: active once the object answers GetPosition: ' .. b)
r = { PR.Tick('') }
check(r[4] == 4 and r[5] == 0x2001, 'Tick reports the buddy type and object')
check(r[6] == 10 and r[7] == 0 and r[8] == -4, 'Tick reports the buddy position')
check(r[9] == nil, 'unknown buddy yaw stays nil')
check(PR.Buddy('quiet', 0, 0, 0, 0):find('^missing') ~= nil, 'buddy not obtained -> missing')
check(PR.Buddy('none', 0, 0, 0, 0) == 'none', 'no buddy configured')

-- ---- mission loading -------------------------------------------------------------------------------
log = {}
env.vars.missionCode = 10040
check(PR.LoadMission(30050, 'direct'):find('^finalize true') ~= nil, 'direct load')
check(env.gvars.mis_nextMissionCodeForMissionClear == 30050 and env.mvars.mis_showLoadingTipsOnMissionFinalize == false, 'direct load sets gvars/mvars')
check(PR.LoadMission(30050, 'reserve'):find('^reserve true') ~= nil and log[2] == 'reserve 7 30050', 'reserve load')
env.vars.missionCode = 40050
log = {}
PR.LoadMission(30010, 'direct')
check(log[1] == 'reserve 7 30010', 'from the ACC (heli space) always reserves: ' .. tostring(log[1]))

-- ---- setup -----------------------------------------------------------------------------------------
log = {}
local s = PR.Setup(1, 2, 3, 90, true, true)
check(s:find('warp=ok') and s:find('disabled=5') and s:find('invincible=true'), 'setup: ' .. s)
check(log[1]:find('WarpAndWaitBlock 1,2,3 1.57') ~= nil, 'setup warp converts degrees to radians: ' .. log[1])
check(PR.Clock('12:00:00', true) == 'time=true freeze=true', 'clock')
check(PR.Weather('SUNNY') == 'weather=true', 'weather')
check(PR.Weather('NOPE'):find('unknown') ~= nil, 'unknown weather')
check(type(PR.Music('mute_bgm')) == 'string', 'music best effort never throws')
local mute = nil
env.TppSoundDaemon = { SetMute = function(n) mute = n end, CheckCurrentMuteIs = function(n) return mute == n end, CheckHighestMuteMoreThan = function() return false end }
env.RadioDaemon = { GetInstance = function() return { DisableRadio = function() record('radio off') end } end }
local a = PR.Audio('off', 'off', 'off')
check(mute == 'Pause' and a:find('mute=true') and a:find('radio=true'), 'audio: Pause mute + radio off: ' .. a)
mute = nil; for _ = 1, 60 do PR.Tick('') end
check(mute == 'Pause', 'audio: Tick re-applies the mute after the pause menu reset it')
check(type(PR.State()) == 'string', 'State() for the console')
local cok, ctext = PR.Console(loadstring('return 1, nil, "x", true'))
check(cok == true and ctext == '1\tnil\tx\ttrue', 'Console packs results: ' .. tostring(ctext))
cok, ctext = PR.Console(loadstring('error("bad")'))
check(cok == false and ctext:find('bad') ~= nil, 'Console reports errors')

-- ---- robustness: an empty engine ---------------------------------------------------------------------
local bare = { math = math, string = string, table = table, pairs = pairs, ipairs = ipairs, type = type, tostring = tostring,
	tonumber = tonumber, pcall = pcall, error = error, select = select, setmetatable = setmetatable }
bare._G = bare
local P2 = load(bare)
local ok, f = pcall(P2.Tick, 'warp 1 2 3 0')
check(ok and f == 0, 'Tick with no engine API: flags 0, no error')
check(pcall(P2.Buddy, 'walker', 0, 0, 0, 0), 'Buddy with no engine API')
check(pcall(P2.LoadMission, 30050, 'direct'), 'LoadMission with no engine API')
check(pcall(P2.Setup, 0, 0, 0, 0, true, true), 'Setup with no engine API')
check(pcall(P2.State), 'State with no engine API')

-- ---- robustness: everything throws -------------------------------------------------------------------
local angry = freshEnv()
for _, mod in pairs({ 'PlayerInfo', 'TppSequence', 'TppMission', 'TppBuddy2BlockController', 'GameObject', 'TppUiCommand', 'Player' }) do
	for k in pairs(angry[mod]) do angry[mod][k] = function() error('boom') end end
end
local P3 = load(angry)
check(pcall(P3.Tick, 'warp 1 2 3 0'), 'Tick survives throwing engine calls')
check(pcall(P3.Buddy, 'walker', 0, 0, 0, 0), 'Buddy survives throwing engine calls')

if failures > 0 then
	print(string.format('test_guest_script: %d FAILED', failures))
	os.exit(1)
end
print('test_guest_script: all passed')
