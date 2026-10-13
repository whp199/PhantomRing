#include "State.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace phantom::guest::state
{
	namespace
	{
		std::mutex                 g_lock;
		LuaView                    g_lua;
		FrameView                  g_frame;
		std::string                g_commands;
		std::optional<logic::Vec3> g_centre;
		std::atomic<int>           g_boot{ static_cast<int>(Boot::kWaitLua) };
		std::atomic<bool>          g_restart{ false };
		std::atomic<bool>          g_treadmill{ true };
		std::atomic<bool>          g_parked{ false };  // Tarnished mode (kHEvTarnished)
		std::atomic<bool>          g_buddyMissing{ false };
	}

	const char* BootName(Boot a_boot)
	{
		switch (a_boot) {
		case Boot::kWaitLua:
			return "wait-lua";
		case Boot::kTitle:
			return "title";
		case Boot::kWaitGame:
			return "wait-game";
		case Boot::kLoadSandbox:
			return "load-sandbox";
		case Boot::kSettle:
			return "settle";
		case Boot::kSetup:
			return "setup";
		case Boot::kBuddy:
			return "buddy";
		case Boot::kBox:
			return "box";
		case Boot::kCentre:
			return "centre";
		case Boot::kReady:
			return "ready";
		default:
			return "failed";
		}
	}

	void SetLua(const LuaView& a_view)
	{
		std::scoped_lock lock(g_lock);
		g_lua = a_view;
	}

	LuaView Lua()
	{
		std::scoped_lock lock(g_lock);
		return g_lua;
	}

	void SetFrame(const FrameView& a_view)
	{
		std::scoped_lock lock(g_lock);
		g_frame = a_view;
	}

	FrameView Frame()
	{
		std::scoped_lock lock(g_lock);
		return g_frame;
	}

	namespace
	{
		std::atomic<LuaThreadWork> g_luaWork{ nullptr };
		std::atomic<LuaThreadWork> g_tickWork{ nullptr };
		std::atomic<CommandResultSink> g_resultSink{ nullptr };
		std::atomic<bool>          g_menuOpen{ false };
	}

	void SetLuaThreadWork(LuaThreadWork a_work) { g_luaWork.store(a_work, std::memory_order_release); }

	void RunLuaThreadWork(std::uint64_t a_nowMs, bool a_ready)
	{
		if (const auto work = g_luaWork.load(std::memory_order_acquire)) {
			work(a_nowMs, a_ready);
		}
	}

	namespace
	{
		BuddyHooks g_buddyHooks;  // set once at activation, before the Lua thread runs
	}

	void SetBuddyHooks(const BuddyHooks& a_hooks) { g_buddyHooks = a_hooks; }
	bool BuddyPose(logic::Vec3& a_pos, float& a_yaw) { return g_buddyHooks.pose && g_buddyHooks.pose(a_pos, a_yaw); }
	void HintBuddy(const logic::Vec3& a_at, float a_radius, std::uint64_t a_forMs)
	{
		if (g_buddyHooks.hint) {
			g_buddyHooks.hint(a_at, a_radius, a_forMs);
		}
	}
	bool WhistleHorse(const char* a_why) { return g_buddyHooks.whistleHorse && g_buddyHooks.whistleHorse(a_why); }
	void ForgetBuddy(const char* a_why)
	{
		if (g_buddyHooks.forget) {
			g_buddyHooks.forget(a_why);
		}
	}

	void SetLuaTickWork(LuaThreadWork a_work) { g_tickWork.store(a_work, std::memory_order_release); }

	void RunLuaTickWork(std::uint64_t a_nowMs, bool a_ready)
	{
		if (const auto work = g_tickWork.load(std::memory_order_acquire)) {
			work(a_nowMs, a_ready);
		}
	}

	void SetCommandResultSink(CommandResultSink a_sink) { g_resultSink.store(a_sink, std::memory_order_release); }

	void RunCommandResults(std::string_view a_results)
	{
		if (const auto sink = g_resultSink.load(std::memory_order_acquire)) {
			sink(a_results);
		}
	}

	void SetMenuOpen(bool a_open) { g_menuOpen.store(a_open, std::memory_order_relaxed); }
	bool MenuOpen() { return g_menuOpen.load(std::memory_order_relaxed); }

	void QueueCommand(const std::string& a_command)
	{
		std::scoped_lock lock(g_lock);
		if (!g_commands.empty()) {
			g_commands += ';';
		}
		g_commands += a_command;
	}

	std::string TakeCommands()
	{
		std::scoped_lock lock(g_lock);
		return std::exchange(g_commands, {});
	}

	void SetBoot(Boot a_boot) { g_boot.store(static_cast<int>(a_boot), std::memory_order_release); }
	Boot CurrentBoot() { return static_cast<Boot>(g_boot.load(std::memory_order_acquire)); }
	namespace
	{
		std::mutex                    g_requestLock;
		std::optional<SandboxRequest> g_sandboxRequest;
		std::optional<HoldRequest>    g_holdRequest;
	}

	void RequestSandbox(const SandboxRequest& a_request)
	{
		std::scoped_lock lock(g_requestLock);
		g_sandboxRequest = a_request;
	}

	std::optional<SandboxRequest> TakeSandboxRequest()
	{
		std::scoped_lock lock(g_requestLock);
		return std::exchange(g_sandboxRequest, std::nullopt);
	}

	void RequestHold(const HoldRequest& a_request)
	{
		std::scoped_lock lock(g_requestLock);
		g_holdRequest = a_request;
	}

	std::optional<HoldRequest> TakeHoldRequest()
	{
		std::scoped_lock lock(g_requestLock);
		return std::exchange(g_holdRequest, std::nullopt);
	}

	namespace
	{
		std::optional<std::string> g_buddyRequest;
	}

	void RequestBuddy(const std::string& a_type)
	{
		std::scoped_lock lock(g_requestLock);
		g_buddyRequest = a_type;
	}

	std::optional<std::string> TakeBuddyRequest()
	{
		std::scoped_lock lock(g_requestLock);
		return std::exchange(g_buddyRequest, std::nullopt);
	}

	void RequestBootRestart() { g_restart.store(true); }
	bool TakeBootRestart() { return g_restart.exchange(false); }

	void SetCentre(std::optional<logic::Vec3> a_centre)
	{
		std::scoped_lock lock(g_lock);
		g_centre = a_centre;
	}

	std::optional<logic::Vec3> Centre()
	{
		std::scoped_lock lock(g_lock);
		return g_centre;
	}

	void SetTreadmillEnabled(bool a_on) { g_treadmill.store(a_on); }
	void SetBuddyMissing(bool a_missing) { g_buddyMissing.store(a_missing); }
	bool BuddyMissing() { return g_buddyMissing.load(); }
	bool TreadmillEnabled() { return g_treadmill.load(); }
	void SetParked(bool a_parked) { g_parked.store(a_parked); }
	bool Parked() { return g_parked.load(); }
}
