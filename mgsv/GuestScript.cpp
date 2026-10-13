// The guest's Lua half, embedded. GuestScript.h explains how it is loaded and overridden.
#include "GuestScript.h"

namespace phantom::guest::script
{
	// clang-format off
	static constexpr char kScript[] = R"PRLUA(-- PhantomRing guest script (Lua 5.1), running inside MGSV on the game's own Lua thread.
--
-- dinput8.dll (PhantomRingGuest) loads it through its lua_pcall depth-1 pump. A file named
-- phantomring.lua next to the DLL replaces it and is reloaded whenever it changes (live iteration;
-- the DLL writes the embedded version to phantomring.default.lua as a starting point).
--
-- C++ calls PhantomRing.Tick(commands) once per game update and reads its results by position
-- (keep the order in sync with mgsv/Guest.cpp TickResult). Boot steps call the other functions.
-- Every engine call is pcall-guarded: a missing or changed API costs one feature, never the game.
-- UNVERIFIED until the first live run: every engine call below except GameObject.SendCommand
-- WarpAndWaitBlock, PlayerInfo.AndCheckStatus, TppBuddy2BlockController.* and gvars.ini_isTitleMode
-- (used by IH / MGS5VR on this build). docs/GUEST.md lists what to check.
local PR = {}
PR.version = 1
PR.tick = 0

local NULL_ID = 65535

-- Flag bits returned by Tick (mirrored in mgsv/Guest.cpp LuaFlags).
PR.F = {
	TITLE = 1, PREPARED = 2, DEMO = 4, MENU = 8, DEAD = 16, VEHICLE = 32, DASH = 64, STAND = 128,
	SQUAT = 256, CRAWL = 512, CQC = 1024, SUBJECT = 2048, FALL = 4096, BINOCLE = 8192,
	NORMAL = 16384, HELI = 32768, GUN_FIRE = 65536, CARRY = 131072, SEQ_GAME = 262144, BEHIND = 524288,
	CBOX = 1048576, POSTER = 2097152, NVG = 4194304,
}

local function fn(mod, name)
	if type(mod) == 'table' then
		local f = mod[name]
		if type(f) == 'function' then return f end
	end
	return nil
end

-- call: results or nil. try: ok flag first (for functions that return nothing).
local function call(mod, name, ...)
	local f = fn(mod, name)
	if not f then return nil end
	local ok, a, b, c, d = pcall(f, ...)
	if ok then return a, b, c, d end
	return nil
end

local function try(mod, name, ...)
	local f = fn(mod, name)
	if not f then return false, 'missing ' .. tostring(name) end
	return pcall(f, ...)
end

local function truthy(v) return v == true or (type(v) == 'number' and v ~= 0) end

local function status(name)
	if type(PlayerStatus) ~= 'table' then return false end
	local v = PlayerStatus[name]
	if type(v) ~= 'number' then return false end
	return call(PlayerInfo, 'AndCheckStatus', {v}) == true
end

local function xyz(v)
	if v == nil then return nil end
	if type(v) == 'table' then
		if type(v[1]) == 'number' then return v[1], v[2], v[3] end
		if type(v.x) == 'number' then return v.x, v.y, v.z end
	end
	local ok, x, y, z = pcall(function() return v:GetX(), v:GetY(), v:GetZ() end)
	if ok and type(x) == 'number' then return x, y, z end
	return nil
end

local function send(target, command)
	local f = fn(GameObject, 'SendCommand')
	if not f then return false, 'no GameObject.SendCommand' end
	return pcall(f, target, command)
end

local function vector(x, y, z)
	if Vector3 == nil then return nil end
	local ok, v = pcall(Vector3, x, y, z)
	if ok then return v end
	return nil
end

local function sequenceName() return call(TppSequence, 'GetCurrentSequenceName') end

-- ---- state -----------------------------------------------------------------------------------
function PR.Flags()
	local F = PR.F
	local f = 0
	local mission = type(vars) == 'table' and vars.missionCode or 0
	if type(gvars) == 'table' and truthy(gvars.ini_isTitleMode) then f = f + F.TITLE end
	if call(TppSequence, 'IsMissionPrepareFinished') == true then f = f + F.PREPARED end
	local seq = sequenceName()
	if type(seq) == 'string' and string.sub(seq, 1, 9) == 'Seq_Game_' then f = f + F.SEQ_GAME end
	if truthy(call(TppMission, 'IsHelicopterSpace', mission)) then f = f + F.HELI end
	if truthy(call(DemoDaemon, 'IsDemoPlaying')) then f = f + F.DEMO end
	if truthy(call(TppUiCommand, 'IsMbDvcTerminalOpened')) or truthy(call(TppUiCommand, 'IsShowPopup')) then f = f + F.MENU end
	if status('DEAD') or status('DEAD_FRESH') then f = f + F.DEAD end
	if status('ON_VEHICLE') or status('ON_HORSE') then f = f + F.VEHICLE end
	if status('DASH') then f = f + F.DASH end
	if status('STAND') then f = f + F.STAND end
	if status('SQUAT') then f = f + F.SQUAT end
	if status('CRAWL') then f = f + F.CRAWL end
	if status('CQC_HOLD') or status('CQC_CONTINUOUS') then f = f + F.CQC end
	if status('SUBJECT') then f = f + F.SUBJECT end
	if status('FALL') then f = f + F.FALL end
	if status('BINOCLE') then f = f + F.BINOCLE end
	if status('NORMAL_ACTION') then f = f + F.NORMAL end
	if status('GUN_FIRE') or status('GUN_FIRE_SUPPRESSOR') then f = f + F.GUN_FIRE end
	if status('CARRY') then f = f + F.CARRY end
	if status('BEHIND') then f = f + F.BEHIND end
	-- Inside a cardboard box (PlayerStatus CBOX; CBOX_EVADE is the dive out of it), and is it one with a poster on it
	-- (vars.cboxPosterIndices[item slot] = the TppCollection poster type, 0 = none; docs/GUEST.md 18.9)
	if status('CBOX') then
		f = f + F.CBOX
		local ok, poster = pcall(function() return vars.cboxPosterIndices[vars.currentItemIndex] end)
		if ok and type(poster) == 'number' and poster ~= 0 then f = f + F.POSTER end
	end
	-- Night-vision goggles on: vars.currentItemIndex is the item slot Snake wears/uses (0 = none; the box's slot while he is in
	-- it), so the goggles are on when that slot holds EQP_IT_Nvg (VERIFIED live: 3 with them on, 0 off, docs/GUEST.md 18.11)
	local okN, nvg = pcall(function()
		local i = vars.currentItemIndex
		return i ~= 0 and vars.items[i] == TppEquip.EQP_IT_Nvg
	end)
	if okN and nvg then f = f + F.NVG end
	return f, mission, seq
end

-- ---- buddies -----------------------------------------------------------------------------------
local BUDDY_TYPE = { walker = 'WALKER_GEAR', horse = 'HORSE', dog = 'DOG', quiet = 'QUIET' }
-- GameObject classes a deployed buddy may be. Which one the sortie D-Walker uses is UNVERIFIED (the
-- exe has TppCommonWalkerGear2 and TppWalkerGear2); the first that resolves wins.
local BUDDY_CLASSES = {
	WALKER_GEAR = { 'TppCommonWalkerGear2', 'TppWalkerGear2' },
	HORSE = { 'TppHorse2' },
	DOG = { 'TppBuddyDog2' },
	QUIET = { 'TppBuddyQuiet2' },
}

local function activeBuddy()
	local active = call(TppBuddy2BlockController, 'GetActiveBuddyType')
	if type(active) ~= 'number' or BuddyType == nil then return nil, active end
	-- BuddyType is an engine enum: indexing works, pairs() enumerates nothing (VERIFIED live).
	for name in pairs(BUDDY_CLASSES) do
		local ok, value = pcall(function() return BuddyType[name] end)
		if ok and value == active then return name, active end
	end
	return nil, active
end

local function buddyObject(name)
	for _, cls in ipairs(BUDDY_CLASSES[name] or {}) do
		local id = call(GameObject, 'GetGameObjectIdByIndex', cls, 0)
		if type(id) == 'number' and id ~= NULL_ID then return id, cls end
	end
	return nil
end

local function objectPosition(id)
	local ok, p = send(id, { id = 'GetPosition' })
	if ok then return xyz(p) end
	return nil
end

-- The active buddy as the ini names it (walker, horse, dog, quiet), or nil: the player may deploy another one from the iDroid
-- (Buddy Support > D-Horse / D-Dog / ...: "Request deployment", live 2026-10-13), and the guest then follows that choice.
local KIND_OF = { WALKER_GEAR = 'walker', HORSE = 'horse', DOG = 'dog', QUIET = 'quiet' }
function PR.ActiveBuddyKind()
	local name = activeBuddy()
	return name and KIND_OF[name] or nil
end

-- Deploys the configured buddy next to (x, y, z). Called repeatedly by the boot sequence until it
-- answers 'active', 'missing:*' or 'unavailable:*'. Mirrors MGS5VR native_cabin.lua (D-Dog in the ACC).
-- revive: a buddy MGSV counts as dead (destroyed in its world) is brought back first (TppBuddyService.UnsetDeadBuddyType; the
-- save is never written while PhantomRing runs).
function PR.Buddy(kind, x, y, z, yaw, revive)
	local typeName = BUDDY_TYPE[kind]
	if not typeName then return 'none' end
	if type(BuddyType) ~= 'table' or type(TppBuddy2BlockController) ~= 'table' then return 'unavailable: no buddy API' end
	local bt = BuddyType[typeName]
	if type(bt) ~= 'number' then return 'unavailable: no BuddyType.' .. typeName end
	if call(TppBuddyService, 'DidObtainBuddyType', bt) == false then return 'missing: not developed in this save' end
	if call(TppBuddyService, 'IsDeadBuddyType', bt) == true then
		if revive then try(TppBuddyService, 'UnsetDeadBuddyType', bt) end
		if call(TppBuddyService, 'IsDeadBuddyType', bt) == true then return 'missing: dead' end
		PR.buddyCalled = nil
		PR.buddyRevived = (PR.buddyRevived or 0) + 1
	end
	if call(TppBuddyService, 'CanSortieBuddyType', bt) == false then return 'missing: cannot sortie' end
	local name, active = activeBuddy()
	if active == bt then
		local id, cls = buddyObject(typeName)
		local bx, by, bz = nil, nil, nil
		if id then bx, by, bz = objectPosition(id) end
		if bx then
			PR.buddyCalled = nil
			-- Already deployed but left behind (the sandbox moved): bring it next to Snake.
			local dx, dz = bx - x, bz - z
			if dx * dx + dz * dz > 30 * 30 then
				local ok = send(id, { id = 'SetPosition', pos = { x, y, z }, rotY = yaw or 0 })
				return 'active ' .. tostring(cls) .. ' ' .. tostring(id) .. ' (moved next to Snake: ' .. tostring(ok) .. ')'
			end
			return 'active ' .. tostring(cls) .. ' ' .. tostring(id)
		end
		if call(TppBuddy2BlockController, 'IsLoading') == true then return 'loading block' end
		if not PR.buddyCalled then
			try(TppBuddyService, 'UnsetDisableCallBuddyType', bt)
			local ok, r = try(TppBuddy2BlockController, 'CallBuddy', bt, vector(x, y, z), yaw or 0, true)
			PR.buddyCalled = PR.tick
			return 'called ' .. tostring(ok) .. ' ' .. tostring(r)
		end
		if PR.tick - PR.buddyCalled > 600 then PR.buddyCalled = nil end  -- call again after ~10 s
		return id and 'spawned (no position yet)' or 'waiting for buddy object'
	end
	if call(TppBuddy2BlockController, 'IsLoading') == true then return 'loading block' end
	-- Asked again when the block didn't come within ~10 s: a mission reload (MGSV's continue after a game over) brings the
	-- save's buddy back and forgets ours (live 2026-10-13: 'waiting for block (active 4)' for D-Horse after one).
	if PR.buddyLoad ~= bt or PR.tick - (PR.buddyLoadTick or 0) > 600 then
		try(TppBuddyService, 'SetSortieBuddyType', bt)
		try(TppBuddy2BlockController, 'SetVarsBuddyType', bt)
		local ok, r = try(TppBuddy2BlockController, 'Load')
		PR.buddyLoad = bt
		PR.buddyLoadTick = PR.tick
		return 'block requested ' .. tostring(ok) .. ' ' .. tostring(r)
	end
	return 'waiting for block (active ' .. tostring(active) .. ')'
end

-- ---- actions -----------------------------------------------------------------------------------
-- Warp: the treadmill (no block wait: the sandbox is streamed; re-backlog M1 -- neither variant fades).
-- WarpAndWaitBlock: boot-time moves to a configured spot that may still need streaming.
function PR.Warp(x, y, z, rotY, waitBlock)
	local id = waitBlock and 'WarpAndWaitBlock' or 'Warp'
	local ok, r = send({ type = 'TppPlayer2', index = 0 }, { id = id, pos = { x, y, z }, rotY = rotY or 0 })
	return ok and 'ok' or ('failed ' .. tostring(r))
end

-- Vehicles and buddies (IH InfTppUtil.Warp: walker gear {pos=..}, vehicles {position=Vector3}).
-- D-Walker takes pos = {x, y, z} only: pos = Vector3(...) or position = Vector3(...) sends it to the world
-- origin, where it is lost (VERIFIED live 2026-10-12). The Vector3 form is tried only for objects that
-- don't answer GetPosition (vehicles, per IH).
function PR.WarpObject(id, x, y, z, rotY)
	if type(id) ~= 'number' or id == NULL_ID then return 'no object' end
	local ok = send(id, { id = 'SetPosition', pos = { x, y, z }, rotY = rotY or 0 })
	local px, py, pz = objectPosition(id)
	if px then
		if (px - x) * (px - x) + (pz - z) * (pz - z) < 1 then return 'ok' end
		return 'not moved (at ' .. string.format('%.1f,%.1f,%.1f', px, py, pz) .. ')'
	end
	local v = vector(x, y, z)
	if v then ok = send(id, { id = 'SetPosition', position = v, rotY = rotY or 0 }) end
	return ok and 'ok (vector form)' or 'failed'
end

-- An Elden Ring hit on Snake, MGSV's way (docs/COMBAT.md): camera noise (keys read from the exe's
-- RequestToSetCameraNoise: levelX, levelY, time, decayRate) and blood on his model where the hit came
-- from (SetBloodToModel: pos, radius). UNVERIFIED live which of the two shows.
function PR.Hurt(level, time, decay, bx, by, bz, radius)
	local out = {}
	if type(level) == 'number' then
		local ok = try(Player, 'RequestToSetCameraNoise', { levelX = level, levelY = level, time = time or 0.4, decayRate = decay or 0.1 })
		out[#out + 1] = 'noise ' .. tostring(ok)
	end
	if type(bx) == 'number' then
		local v = vector(bx, by, bz)
		local ok = v ~= nil and try(Player, 'SetBloodToModel', { pos = v, radius = radius or 0.2 })
		out[#out + 1] = 'blood ' .. tostring(ok)
	end
	return table.concat(out, ' ')
end

function PR.Run(commands)
	local out = {}
	for command in string.gmatch(commands, '[^;]+') do
		local w = {}
		for t in string.gmatch(command, '%S+') do w[#w + 1] = t end
		local n = function(i) return tonumber(w[i]) end
		local ok, r = pcall(function()
			if w[1] == 'warp' then return PR.Warp(n(2), n(3), n(4), n(5), false) end
			if w[1] == 'warpwait' then return PR.Warp(n(2), n(3), n(4), n(5), true) end
			if w[1] == 'owarp' then return PR.WarpObject(n(2), n(3), n(4), n(5), n(6)) end
			-- MGSV's camera orientation (rotX = -pitch, rotY = world yaw, degrees; interpTime 0 is ignored): riding,
			-- the camera is turned back to where it looked before the vehicle's treadmill warp (Frame.cpp)
			if w[1] == 'camrot' then return tostring((try(Player, 'RequestToSetCameraRotation', { rotX = n(2), rotY = n(3), interpTime = n(4) or 0.02 }))) end
			if w[1] == 'hurt' then return PR.Hurt(n(2), n(3), n(4), n(5), n(6), n(7), n(8)) end
			-- PlayerStance value (1 stand, 2 squat, 3 crawl): Snake's death collapse and getting up (Death.cpp)
			if w[1] == 'stance' then return tostring((try(Player, 'RequestToSetTargetStance', n(2)))) end
			-- Rest at a grace / respawn (kHEvRested): the magazine sizes of Snake's three weapons; the C++ side (Ammo.cpp) then
			-- fills the magazines and every ammo/item stock natively, through MGSV's own setters
			if w[1] == 'refill' then return PR.Magazines() end
			return 'unknown'
		end)
		out[#out + 1] = tostring(w[1]) .. '=' .. tostring(ok and r or ('error ' .. tostring(r)))
	end
	return table.concat(out, ' ')
end

-- 'equip/magazine/sub-magazine' of weapon slots 0-2 (PRIMARY_HIP, PRIMARY_BACK, SECONDARY), comma separated: TppEquip.GetAmmoInfo
-- answers bullet, magazine, stock, sub bullet, sub magazine, sub stock (TppPlayer.SupplyWeaponAmmoFull reads it the same way)
function PR.Magazines()
	local out = {}
	for slot = 0, 2 do
		local ok, id = pcall(function() return vars.weapons[slot] end)
		if not ok or type(id) ~= 'number' then id = 0 end
		local mag, sub = 0, 0
		if id ~= 0 then
			local okInfo, _, m, _, _, sm = pcall(TppEquip.GetAmmoInfo, id)
			if okInfo then mag, sub = tonumber(m) or 0, tonumber(sm) or 0 end
		end
		out[#out + 1] = string.format('%d/%d/%d', id, mag, sub)
	end
	return table.concat(out, ',')
end

function PR.LoadMission(code, method)
	if type(TppMission) ~= 'table' or type(gvars) ~= 'table' then return 'no TppMission' end
	local current = type(vars) == 'table' and vars.missionCode or 0
	if method == 'reserve' or truthy(call(TppMission, 'IsHelicopterSpace', current)) then
		local clear = type(TppDefine) == 'table' and type(TppDefine.MISSION_CLEAR_TYPE) == 'table' and TppDefine.MISSION_CLEAR_TYPE.FROM_HELISPACE or 7
		local ok, r = try(TppMission, 'ReserveMissionClear', { missionClearType = clear, nextMissionId = code })
		return 'reserve ' .. tostring(ok) .. ' ' .. tostring(r)
	end
	-- Infinite Heaven's direct load (InfMission loadDirect)
	gvars.mis_nextMissionCodeForMissionClear = code
	if type(mvars) == 'table' then mvars.mis_showLoadingTipsOnMissionFinalize = false end
	local ok, r = try(TppMission, 'ExecuteMissionFinalize')
	return 'finalize ' .. tostring(ok) .. ' ' .. tostring(r)
end

function PR.EndLoadingTips()
	if truthy(call(TppUiCommand, 'IsEndLoadingTips')) then return 'ended' end
	try(TppUiCommand, 'PermitEndLoadingTips')
	local ok = try(TppUiCommand, 'RequestEndLoadingTips')
	return 'requested ' .. tostring(ok)
end

function PR.DisableSoldiers()
	local f = fn(GameObject, 'GetGameObjectIdByIndex')
	if not f then return 'no GetGameObjectIdByIndex' end
	local count = 0
	for _, cls in ipairs({ 'TppSoldier2', 'TppHostage2' }) do
		local misses = 0
		for i = 0, 1023 do
			local ok, id = pcall(f, cls, i)
			if not ok or type(id) ~= 'number' or id == NULL_ID then
				misses = misses + 1
				if misses > 64 then break end
			else
				misses = 0
				if send(id, { id = 'SetEnabled', enabled = false }) then count = count + 1 end
			end
		end
	end
	return tostring(count)
end

-- MGSV's markers off: a marked MGSV soldier near the sandbox (they stay standing, disabled) showed in Elden Ring's HUD layer and
-- MGSV's aim snapped onto it (live: a rocket launcher's aim stuck 30 deg off the Elden Ring target until TppMarker.DisableAll).
function PR.ClearMarkers()
	local ok = try(TppMarker, 'DisableAll')
	return tostring(ok)
end

-- MGSV's "Auto-Aim Correction" (OPTIONS > Control Settings, its first row) is vars.optionSelectedIndices[11]: 0 ON, 1 OFF (VERIFIED
-- live: applying it in the menu flips only that byte, 0x142C14509; with 1 an FB MR aimed 3-25 deg beside a disabled soldier no
-- longer swings onto him). The game reads it directly. Written in memory only: MGSV's config save is blocked (SaveGuard).
function PR.AutoAim(on)
	local ok, cur = pcall(function() return vars.optionSelectedIndices[11] end)
	if not ok or (cur ~= 0 and cur ~= 1) then return 'not changed (option 11 is ' .. tostring(cur) .. ')' end
	local want = on and 0 or 1
	local okSet = cur == want or pcall(function() vars.optionSelectedIndices[11] = want end)
	return string.format('%s (was %s)%s', want == 0 and 'on' or 'off', cur == 0 and 'on' or 'off', okSet and '' or ' -- write failed')
end

function PR.Setup(x, y, z, rotYDeg, disableSoldiers, invincible)
	local out = {}
	if x then out[#out + 1] = 'warp=' .. PR.Warp(x, y, z, math.rad(rotYDeg or 0), true) end
	if disableSoldiers then out[#out + 1] = 'disabled=' .. PR.DisableSoldiers() end
	out[#out + 1] = 'markers off=' .. PR.ClearMarkers()
	if invincible then
		local ok = try(TppGameStatus, 'Set', 'PhantomRing', 'S_DISABLE_PLAYER_DAMAGE')
		out[#out + 1] = 'invincible=' .. tostring(ok)
	end
	return table.concat(out, ' ')
end

function PR.Clock(hms, freeze)
	local out = {}
	if hms and hms ~= '' then out[#out + 1] = 'time=' .. tostring((try(TppClock, 'SetTime', hms))) end
	if freeze then
		local weather = type(TppCommand) == 'table' and TppCommand.Weather or nil
		out[#out + 1] = 'freeze=' .. tostring((try(weather, 'SetClockTimeScale', 0)))
	end
	return table.concat(out, ' ')
end

function PR.Weather(name)
	if not name or name == '' then return 'kept' end
	local w = type(TppDefine) == 'table' and type(TppDefine.WEATHER) == 'table' and TppDefine.WEATHER[name] or nil
	if type(w) ~= 'number' then return 'unknown weather ' .. tostring(name) end
	return 'weather=' .. tostring((try(TppWeather, 'ForceRequestWeather', w, 0)))
end

-- Best effort: the engine's music API isn't mapped yet. Reports which calls existed.
function PR.Music(mode)
	if mode == 'keep' then return 'kept' end
	local out = {}
	out[#out + 1] = 'StopSceneBGM=' .. tostring((try(TppSound, 'StopSceneBGM')))
	out[#out + 1] = 'StopMusicPlayer=' .. tostring((try(TppMusicManager, 'StopMusicPlayer', 1)))
	if mode == 'mute_all' then
		out[#out + 1] = 'SetMute=' .. tostring((try(TppSoundDaemon, 'SetMute', 'Pause')))
	end
	return table.concat(out, ' ')
end

-- MGSV's sound next to Elden Ring's (docs/GUEST.md "Audio"). Snake's own sounds (guns, reloads, footsteps, CQC,
-- his voice), D-Walker's and MGSV's UI blips always play. VERIFIED live 2026-10-12 (parecord on MGSV's stream):
--  * SetMute('Pause') -- the mute MGSV's own pause menu sets -- silences music AND ambience (wind, birds,
--    insects) and keeps the player's sounds and UI blips. The pause menu resets it when it closes, so
--    PR.AudioKeep re-applies it (never over a stronger mute such as Loading/Title: CheckHighestMuteMoreThan).
--    It cannot separate the two: Ambience=off mutes the music too.
--  * Music only: the scene BGM / music player stops (best effort: phase BGM can come back).
--  * Radio: RadioDaemon:DisableRadio() keeps radio calls (Kaz, Ocelot, mission radio) from playing; the iDroid's
--    voice announcements: TppUiCommand.SetAllInvalidMbSoundControllerVoice() (UNVERIFIED: no way found to
--    trigger one on demand).
function PR.Audio(music, ambience, radio)
	PR.audio = { music = music, ambience = ambience, radio = radio }
	local out = {}
	if music == 'off' then
		out[#out + 1] = 'bgm=' .. tostring((try(TppSound, 'StopSceneBGM'))) .. '/' .. tostring((try(TppMusicManager, 'StopMusicPlayer', 1)))
	end
	out[#out + 1] = PR.AudioKeep(true)
	return table.concat(out, ' ')
end

-- Boot reached Ready: post the mute again. The one posted during setup can be state-true and still not heard
-- (VERIFIED live: CheckCurrentMuteIs('Pause') true, ambience at -37..-44 dBFS). ResetMute and SetMute in the same
-- update don't fix it (the unmute wins), nor a SetMute a few updates later (VERIFIED at boot: still -44 dBFS);
-- ResetMute now and SetMute >= 90 updates (~1.5 s) later does (the unmute's fade has finished by then).
-- At Ready the refresh alone isn't enough either (VERIFIED at boot: -44 dBFS on after it; the same refresh a minute
-- later silences it): boot passes 'boot' and it runs again ~10 s and ~40 s later.
function PR.AudioRefresh(when)
	if when == 'boot' then PR.audioRefreshAt = { PR.tick + 600, PR.tick + 2400 } end
	local a = PR.audio
	if not a or a.ambience ~= 'off' then return PR.AudioKeep(true) end
	if call(TppSoundDaemon, 'CheckCurrentMuteIs', 'Pause') == true then
		try(TppSoundDaemon, 'ResetMute', 'Pause')
		PR.audioHoldUntil = PR.tick + 90
		return 'reset; muted again in ~1.5 s'
	end
	return PR.AudioKeep(true)
end

function PR.AudioKeep(report)
	local a = PR.audio
	if not a then return 'none' end
	local out = {}
	if a.ambience == 'off' and PR.tick >= (PR.audioHoldUntil or 0) then
		local muted = call(TppSoundDaemon, 'CheckCurrentMuteIs', 'Pause') == true
		-- MoreThan means "at least" (VERIFIED: true while Pause itself is the highest)
		local stronger = call(TppSoundDaemon, 'CheckHighestMuteMoreThan', 'Pause') == true and call(TppSoundDaemon, 'CheckHighestMuteIs', 'Pause') ~= true
		if not muted and not stronger then
			local ok = try(TppSoundDaemon, 'SetMute', 'Pause')
			PR.audioMutes = (PR.audioMutes or 0) + 1
			out[#out + 1] = 'mute=' .. tostring(ok)
		elseif report then
			out[#out + 1] = muted and 'mute=already' or 'mute=later (stronger mute active)'
		end
	end
	if a.radio == 'off' then
		local d = call(RadioDaemon, 'GetInstance')
		local ok = d ~= nil and pcall(function() d:DisableRadio() end)
		local ok2 = try(TppUiCommand, 'SetAllInvalidMbSoundControllerVoice')
		if report then out[#out + 1] = 'radio=' .. tostring(ok) .. ' idroid=' .. tostring(ok2) end
	end
	return table.concat(out, ' ')
end

-- MGSV's own menus in the sandbox (docs/GUEST.md 18.6). The pause menu's page is MGSV's own registration call, re-done with
-- only the harmless items: RESTART_FROM_CHECK_POINT, ABORT_MISSION_RETURN_TO_ACC and RETURN_TO_TITLE would end the sandbox
-- (the boot sequence recovers, minutes later). GamePauseMenu is an engine enum: indexing works, pairs() doesn't.
PR.SAFE_PAUSE_ITEMS = { 'CONTROLS_AND_TIPS_ITEM', 'OPEN_OPTION_MENU' }

function PR.Menus(safePause, blockMissions)
	local out = {}
	if safePause then
		if type(GamePauseMenu) ~= 'table' and type(GamePauseMenu) ~= 'userdata' then
			out[#out + 1] = 'pause=no GamePauseMenu'
		else
			local items = {}
			for _, name in ipairs(PR.SAFE_PAUSE_ITEMS) do
				local ok, v = pcall(function() return GamePauseMenu[name] end)
				if ok and type(v) == 'number' then items[#items + 1] = v end
			end
			out[#out + 1] = 'pause=' .. tostring((try(TppUiCommand, 'RegisterPauseMenuPage', items))) .. '(' .. #items .. ' items)'
		end
	end
	if blockMissions then out[#out + 1] = 'missions=' .. PR.BlockMissions() end
	return table.concat(out, ' ')
end

-- The iDroid's mission list, helicopter and location change would leave the sandbox. TppTerminal (MGSV's own script)
-- names the iDroid's top menu items; SetMbTopMenuItemActive greys them out.
PR.BLOCKED_IDROID = { 'MSN_MISSIONLIST', 'MSN_SIDEOPSLIST', 'MSN_LOCATION', 'MSN_HELI', 'MSN_HELI_RENDEZVOUS', 'MSN_RETURNMB', 'MSN_HELI_DISMISS',
	'MSN_FOB', 'MSN_EMERGENCIE_N', 'MSN_EMERGENCIE_F', 'MSN_BUDDY_WALKER_DISMISS', 'MSN_BUDDY_HORSE_DISMISS', 'MSN_BUDDY_DOG_DISMISS',
	'MSN_BUDDY_QUIET_DISMISS', 'MSN_BUDDY_BATTLE_DISMISS' }

function PR.BlockMissions()
	local menu = type(TppTerminal) == 'table' and TppTerminal.MBDVCMENU or nil
	if type(menu) ~= 'table' then return 'no TppTerminal.MBDVCMENU' end
	local done, missing = 0, 0
	for _, name in ipairs(PR.BLOCKED_IDROID) do
		local item = menu[name]
		if item ~= nil and try(TppUiCommand, 'SetMbTopMenuItemActive', item, false) then done = done + 1 else missing = missing + 1 end
	end
	return done .. ' blocked, ' .. missing .. ' missing'
end

-- [Loadout] weapons (TppEquip.EQP_* names; '' keeps the save's) go into the sortie loadout vars BEFORE the sandbox mission
-- loads (vars.initWeapons / vars.weapons, PRIMARY_1 0 hip, PRIMARY_2 1 back, SECONDARY 2; magazine from
-- TppEquip.GetAmmoInfo(id) = (?, magazine, max stock, ...)): MGSV then loads their models with the mission and fills their
-- ammo stock itself. VERIFIED live 2026-10-12: the WU S.PISTOL (EQP_WP_West_thg_010) written this way survived a direct
-- reload of the sandbox and Snake drew, aimed, scoped and fired it (bullet 10 BL_HgTranq). Given mid-mission
-- (Player.ChangeEquip) a weapon whose model the mission didn't load shows in the weapon box but can't be drawn, and
-- hooking the mission's equip list hung the next load. Returns the text and how many slots it changed.
PR.LOADOUT_SLOTS = { 0, 1, 2 }

local function weaponIds(primary, primary2, secondary)
	local names = { primary or '', primary2 or '', secondary or '' }
	local ids = {}
	for i, name in ipairs(names) do
		if name ~= '' then
			local id = type(TppEquip) == 'table' and TppEquip[name] or nil
			ids[i] = type(id) == 'number' and id or false
		end
	end
	return names, ids
end

function PR.PreLoadout(primary, primary2, secondary)
	if type(vars) ~= 'table' or type(TppEquip) ~= 'table' then return 'no vars', 0 end
	local names, ids = weaponIds(primary, primary2, secondary)
	local out, changed = {}, 0
	for i, slot in ipairs(PR.LOADOUT_SLOTS) do
		local id = ids[i]
		if id == false then
			out[#out + 1] = names[i] .. '=unknown name'
		elseif id then
			local ok, have = pcall(function() return vars.weapons[slot] end)
			if ok and have == id then
				out[#out + 1] = names[i] .. '=already'
			else
				local magazine = 1
				local okInfo, _, mag = pcall(function() return TppEquip.GetAmmoInfo(id) end)
				if okInfo and type(mag) == 'number' and mag > 0 then magazine = mag end
				local okSet, err = pcall(function()
					vars.initWeapons[slot] = id
					vars.weapons[slot] = id
					vars.ammoInWeapons[slot] = magazine
				end)
				if okSet then changed = changed + 1 end
				out[#out + 1] = names[i] .. '(' .. id .. ')=' .. (okSet and ('slot ' .. slot .. ' was ' .. tostring(have)) or ('failed ' .. tostring(err)))
			end
		end
	end
	return (#out > 0 and table.concat(out, ' ') or 'kept'), changed
end

-- How many [Loadout] weapons the loaded mission doesn't have (0 = all there).
function PR.LoadoutMissing(primary, primary2, secondary)
	if type(vars) ~= 'table' or type(TppEquip) ~= 'table' then return 0 end
	local _, ids = weaponIds(primary, primary2, secondary)
	local missing = 0
	for i, slot in ipairs(PR.LOADOUT_SLOTS) do
		if ids[i] then
			local ok, have = pcall(function() return vars.weapons[slot] end)
			if ok and type(have) == 'number' and have ~= ids[i] then missing = missing + 1 end
		end
	end
	return missing
end

-- [Loadout] Support (comma list, up to 8): MGSV's own Player.ChangeEquip{slotType = PlayerSlotType.SUPPORT, ...} at the setup
-- (grenades and mines need no model reload: VERIFIED live). Also the console's way to give anything mid-mission.
function PR.Loadout(primary, primary2, secondary, support)
	if type(Player) ~= 'table' or type(PlayerSlotType) ~= 'table' or type(TppEquip) ~= 'table' then return 'no equip API' end
	local out = {}
	local function give(slot, name, sub)
		if type(name) ~= 'string' or name == '' then return end
		local id = TppEquip[name]
		if type(id) ~= 'number' then out[#out + 1] = name .. '=unknown name' return end
		local ammo, stock = 999, 999
		local okInfo, _, magazine, maxStock = pcall(function() return TppEquip.GetAmmoInfo(id) end)
		if okInfo and type(magazine) == 'number' and magazine > 0 then ammo = magazine end
		if okInfo and type(maxStock) == 'number' and maxStock > 0 then stock = maxStock end
		local ok, r = try(Player, 'ChangeEquip', { slotType = PlayerSlotType[slot], subIndex = sub, equipId = id, stock = stock, stockSub = 0,
			ammo = ammo, ammoSub = 0, suppressorLife = 100, isSuppressorOn = false, isLightOn = false, toActive = false, dropPrevEquip = false })
		out[#out + 1] = name .. '(' .. id .. ')=' .. (ok and 'ok' or ('failed ' .. tostring(r)))
	end
	give('PRIMARY_1', primary)
	give('PRIMARY_2', primary2)
	give('SECONDARY', secondary)
	if type(support) == 'string' and support ~= '' then
		local i = 0
		for name in string.gmatch(support, '[^,%s]+') do
			if i < 8 then give('SUPPORT', name, i) end
			i = i + 1
		end
	end
	return #out > 0 and table.concat(out, ' ') or 'kept'
end

-- ---- cardboard box posters (docs/GUEST.md 18.9) --------------------------------------------------------------------
-- A box's poster is vars.cboxPosterIndices[item slot] = a TppCollection poster type (0 none). The player picks one in
-- MGSV's item menu (hold D-pad left, X "Change Poster"), which cycles through the posters the save has, in type order,
-- then none. Having one = the Mother Base resource Poster1000 + (type - 47) > 0 (VERIFIED live: this save has 1000, 1001,
-- 1003, 1004; the menu offered exactly those). Written directly the var changes nothing on the box, and a mission load
-- clears it: only the menu's X applies a poster, so the setup plays that menu (C++ pad sequence).
PR.POSTERS = { sol_afgn = 47, sol_mafr = 48, sol_zrs = 49, gravure_v = 50, gravure_h = 51, moe_v = 52, moe_h = 53 }
PR.CBOX_FIRST, PR.CBOX_LAST = 456, 474  -- TppEquip.EQP_IT_CBox .. EQP_IT_CBox_LIMITED_G01

local function posterCount(t)
	return call(TppMotherBaseManagement, 'GetResourceUsableCount', { resource = 'Poster' .. tostring(1000 + t - 47) }) or 0
end

local function boxSlot()
	for i = 0, 7 do
		local ok, id = pcall(function() return vars.items[i] end)
		if ok and type(id) == 'number' and id >= PR.CBOX_FIRST and id <= PR.CBOX_LAST then return i, id end
	end
	return nil
end

-- One step towards a box with that poster: 'done <slot>', 'adding' (the poster resource was asked for: MGSV adds it a
-- moment later, in memory only -- saves are blocked), 'box <slot>' (a box was put into a free item slot), 'press <slot>
-- <n>' (n X presses in the item menu with that slot current), or why not.
function PR.Poster(name, boxName)
	local want = PR.POSTERS[name or '']
	if not want then return 'unknown poster ' .. tostring(name) end
	if type(vars) ~= 'table' or type(TppMotherBaseManagement) ~= 'table' then return 'no vars' end
	if posterCount(want) < 1 then
		local ok = try(TppMotherBaseManagement, 'DirectAddResource', { resource = 'Poster' .. tostring(1000 + want - 47), count = 1, isNew = false })
		return ok and 'adding' or 'no DirectAddResource'
	end
	local slot = boxSlot()
	if not slot then
		local id = type(TppEquip) == 'table' and TppEquip[boxName or ''] or nil
		if type(id) ~= 'number' then return 'no box (and no box to give: ' .. tostring(boxName) .. ')' end
		for i = 0, 7 do
			if vars.items[i] == 0 then
				local ok = try(Player, 'ChangeEquip', { slotType = PlayerSlotType.ITEM, subIndex = i, equipId = id, toActive = false })
				return ok and ('box ' .. i) or 'ChangeEquip failed'
			end
		end
		return 'no free item slot for a box'
	end
	local have = vars.cboxPosterIndices[slot]
	if have == want then return 'done ' .. slot end
	-- The menu's cycle: the posters the save has, in type order, then none.
	local cycle = {}
	for t = 47, 53 do if posterCount(t) > 0 then cycle[#cycle + 1] = t end end
	cycle[#cycle + 1] = 0
	local from, to
	for i, t in ipairs(cycle) do
		if t == have then from = i end
		if t == want then to = i end
	end
	if not to then return 'poster not in the cycle' end
	return 'press ' .. slot .. ' ' .. tostring(((to - (from or #cycle)) % #cycle))
end

-- The box in that item slot made Snake's current item (he gets into it: the item menu then opens on it), or the previous
-- current item back (0 = none: RT is CQC again, not the box).
function PR.BoxItem(slot)
	if slot < 0 then
		local prev = PR.itemBeforeBox or 0
		PR.itemBeforeBox = nil
		local ok = pcall(function() vars.currentItemIndex = prev end)
		return 'item ' .. prev .. ' ' .. tostring(ok)
	end
	if PR.itemBeforeBox == nil then PR.itemBeforeBox = vars.currentItemIndex end
	local ok = try(Player, 'ChangeEquip', { slotType = PlayerSlotType.ITEM, subIndex = slot, equipId = vars.items[slot], toActive = true })
	return 'box ' .. slot .. ' ' .. tostring(ok)
end

function PR.State()
	local f, mission, seq = PR.Flags()
	local name, active = activeBuddy()
	return string.format('mission=%s seq=%s flags=%d title=%s buddy=%s(%s) vehicle=%s pos=%.2f,%.2f,%.2f life=%s/%s',
		tostring(mission), tostring(seq), f, tostring(type(gvars) == 'table' and gvars.ini_isTitleMode),
		tostring(name), tostring(active), tostring(vars and vars.playerVehicleGameObjectId),
		vars and vars.playerPosX or 0, vars and vars.playerPosY or 0, vars and vars.playerPosZ or 0,
		tostring(vars and vars.playerLife), tostring(vars and vars.playerLifeMax))
end

-- Dev console: runs a chunk the C++ side compiled; returns ok and every result tostring'd, tab-separated.
function PR.Console(f)
	local r = { pcall(f) }
	if not r[1] then return false, tostring(r[2]) end
	local out = {}
	for i = 2, table.maxn(r) do out[#out + 1] = tostring(r[i]) end
	return true, table.concat(out, '\t')
end

-- ---- per update ----------------------------------------------------------------------------------
-- Results (by position):
--  1 flags  2 missionCode  3 sequence name  4 active buddy type (number or nil)  5 buddy object id
--  6..8 buddy position  9 buddy yaw (nil: unknown; reserved)  10 object id Snake rides (or nil)  11 command results
function PR.Tick(commands)
	PR.tick = PR.tick + 1
	local result = ''
	if type(commands) == 'string' and commands ~= '' then result = PR.Run(commands) end
	if PR.audio and (PR.tick % 60 == 0 or PR.tick == PR.audioHoldUntil) then PR.AudioKeep(false) end
	if PR.audioRefreshAt and (PR.tick == PR.audioRefreshAt[1] or PR.tick == PR.audioRefreshAt[2]) then PR.AudioRefresh() end
	local flags, mission, seq = PR.Flags()
	local name, active = activeBuddy()
	local bid, bx, by, bz, byaw
	if name then
		if not PR.buddyId or PR.tick % 60 == 0 then PR.buddyId = buddyObject(name) end
		bid = PR.buddyId
		if bid then bx, by, bz = objectPosition(bid) end
		-- No per-frame GetRotationY: only one handler in the exe knows it (0x140E86DBE, not a walker as far
		-- as we know). The guest derives the buddy's heading from its motion instead.
	else
		PR.buddyId = nil
	end
	local ridden = call(Player, 'GetGameObjectIdIsRiddenToLocal')
	if type(ridden) ~= 'number' or ridden == NULL_ID then ridden = nil end
	return flags, mission, seq, active, bid, bx, by, bz, byaw, ridden, result
end

PhantomRing = PR
return PR.version
)PRLUA";
	// clang-format on

	std::string_view Embedded() { return { kScript, sizeof(kScript) - 1 }; }
}
