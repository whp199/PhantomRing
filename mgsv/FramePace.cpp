#include "FramePace.h"

#include "Addresses.h"
#include "Config.h"
#include "Hook.h"
#include "Link.h"
#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <array>
#include <atomic>
#include <format>
#include <vector>

namespace phantom::guest::framepace
{
	namespace
	{
		using DelayFn = LONG(NTAPI*)(BOOLEAN, const LARGE_INTEGER*);
		using WaitOneFn = LONG(NTAPI*)(HANDLE, BOOLEAN, const LARGE_INTEGER*);
		using WaitManyFn = LONG(NTAPI*)(ULONG, const HANDLE*, int, BOOLEAN, const LARGE_INTEGER*);
		using AlertWaitFn = LONG(NTAPI*)(const void*, const LARGE_INTEGER*);  // condition variables, SRW locks, WaitOnAddress

		DelayFn             g_realDelay{ nullptr };
		WaitOneFn           g_realWaitOne{ nullptr };
		WaitManyFn          g_realWaitMany{ nullptr };
		AlertWaitFn         g_realAlertWait{ nullptr };
		std::atomic<bool>   g_recording{ false };
		std::atomic<DWORD>  g_gameTid{ 0 };
		std::atomic<DWORD>  g_presentTid{ 0 };
		std::int64_t        g_freq{ 0 };
		thread_local bool   t_inHook{ false };

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			::QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double Ms(std::int64_t a_ticks) { return g_freq ? static_cast<double>(a_ticks) * 1000.0 / static_cast<double>(g_freq) : 0.0; }

		// A spin lock: the hooks below sit under every wait, so they must never wait themselves.
		std::atomic_flag g_spin = ATOMIC_FLAG_INIT;
		struct Spin
		{
			Spin()
			{
				while (g_spin.test_and_set(std::memory_order_acquire)) {
				}
			}
			~Spin() { g_spin.clear(std::memory_order_release); }
		};

		// A window of recent values (ms) with summary statistics.
		struct Window
		{
			static constexpr std::size_t kN = 600;
			std::array<double, kN> v{};
			std::size_t            n{ 0 }, next{ 0 };
			void Add(double a_x)
			{
				v[next] = a_x;
				next = (next + 1) % kN;
				n = std::min(n + 1, kN);
			}
			[[nodiscard]] std::string Describe() const
			{
				if (!n) {
					return "none";
				}
				std::vector<double> s(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
				std::sort(s.begin(), s.end());
				double sum = 0;
				for (double x : s) {
					sum += x;
				}
				auto q = [&](double a_q) { return s[static_cast<std::size_t>(a_q * static_cast<double>(n - 1) + 0.5)]; };
				return std::format("n {} mean {:.2f} p5 {:.2f} p50 {:.2f} p95 {:.2f} max {:.2f}", n, sum / static_cast<double>(n), q(0.05), q(0.5), q(0.95), s.back());
			}
		};

		// Where the game thread blocks: one entry per (kind, call chain).
		struct WaitSite
		{
			DWORD         tid{ 0 };
			int           kind{ 0 };  // 0 delay, 1 wait one, 2 wait many, 3 alert wait (condition variable / SRW / WaitOnAddress)
			void*         chain[8]{};
			std::uint64_t calls{ 0 };
			double        askedMs{ 0 };  // delays / timeouts asked (relative only)
			double        waitedMs{ 0 };
			double        maxMs{ 0 };
		};

		std::vector<WaitSite> g_sites;
		Window                g_gameFrame, g_presentInterval, g_presentCall, g_ours, g_gameWaitPerFrame;
		std::int64_t          g_lastGame{ 0 }, g_lastPresent{ 0 };
		double                g_waitThisFrame{ 0 };
		std::array<std::uint64_t, 5> g_sync{};

		std::atomic<bool> g_allThreads{ false };  // `!pace start all`: every thread, not just the game thread

		bool Watch()
		{
			if (!g_recording.load(std::memory_order_relaxed) || t_inHook) {
				return false;
			}
			const DWORD tid = ::GetCurrentThreadId();
			return g_allThreads.load(std::memory_order_relaxed) || tid == g_gameTid.load(std::memory_order_relaxed) || tid == g_presentTid.load(std::memory_order_relaxed);
		}

		void Note(int a_kind, const LARGE_INTEGER* a_timeout, std::int64_t a_t0, void* const* a_chain, USHORT a_got)
		{
			const double waited = Ms(Qpc() - a_t0);
			const double asked = a_timeout && a_timeout->QuadPart < 0 ? static_cast<double>(-a_timeout->QuadPart) / 10000.0 : -1.0;
			const DWORD  tid = ::GetCurrentThreadId();
			Spin         lock;
			if (tid == g_gameTid.load(std::memory_order_relaxed)) {
				g_waitThisFrame += waited;
			}
			WaitSite* site = nullptr;
			for (auto& s : g_sites) {
				if (s.tid == tid && s.kind == a_kind && std::equal(std::begin(s.chain), std::end(s.chain), a_chain)) {
					site = &s;
					break;
				}
			}
			if (!site) {
				if (g_sites.size() >= 256) {
					return;
				}
				site = &g_sites.emplace_back();
				site->tid = tid;
				site->kind = a_kind;
				std::copy_n(a_chain, std::min<USHORT>(8, a_got), site->chain);
			}
			++site->calls;
			site->askedMs += std::max(0.0, asked);
			site->waitedMs += waited;
			site->maxMs = std::max(site->maxMs, waited);
		}

		LONG NTAPI DelayHook(BOOLEAN a_alertable, const LARGE_INTEGER* a_timeout)
		{
			if (!Watch()) {
				return g_realDelay(a_alertable, a_timeout);
			}
			t_inHook = true;
			void*        chain[8]{};
			const USHORT got = ::RtlCaptureStackBackTrace(1, 8, chain, nullptr);
			const auto   t0 = Qpc();
			const LONG   r = g_realDelay(a_alertable, a_timeout);
			Note(0, a_timeout, t0, chain, got);
			t_inHook = false;
			return r;
		}

		LONG NTAPI WaitOneHook(HANDLE a_h, BOOLEAN a_alertable, const LARGE_INTEGER* a_timeout)
		{
			if (!Watch()) {
				return g_realWaitOne(a_h, a_alertable, a_timeout);
			}
			t_inHook = true;
			void*        chain[8]{};
			const USHORT got = ::RtlCaptureStackBackTrace(1, 8, chain, nullptr);
			const auto   t0 = Qpc();
			const LONG   r = g_realWaitOne(a_h, a_alertable, a_timeout);
			Note(1, a_timeout, t0, chain, got);
			t_inHook = false;
			return r;
		}

		LONG NTAPI WaitManyHook(ULONG a_n, const HANDLE* a_h, int a_type, BOOLEAN a_alertable, const LARGE_INTEGER* a_timeout)
		{
			if (!Watch()) {
				return g_realWaitMany(a_n, a_h, a_type, a_alertable, a_timeout);
			}
			t_inHook = true;
			void*        chain[8]{};
			const USHORT got = ::RtlCaptureStackBackTrace(1, 8, chain, nullptr);
			const auto   t0 = Qpc();
			const LONG   r = g_realWaitMany(a_n, a_h, a_type, a_alertable, a_timeout);
			Note(2, a_timeout, t0, chain, got);
			t_inHook = false;
			return r;
		}

		LONG NTAPI AlertWaitHook(const void* a_address, const LARGE_INTEGER* a_timeout)
		{
			if (!Watch()) {
				return g_realAlertWait(a_address, a_timeout);
			}
			t_inHook = true;
			void*        chain[8]{};
			const USHORT got = ::RtlCaptureStackBackTrace(1, 8, chain, nullptr);
			const auto   t0 = Qpc();
			const LONG   r = g_realAlertWait(a_address, a_timeout);
			Note(3, a_timeout, t0, chain, got);
			t_inHook = false;
			return r;
		}

		// Present experiments: MGSV's sync interval overridden (-1 = its own), our own frame limiter (0 = off), optionally
		// on Elden Ring's frame grid (HostState: its frame number and QPC each frame).
		std::atomic<int>    g_syncOverride{ -1 };
		std::atomic<double> g_limitHz{ 0.0 };
		std::atomic<bool>   g_genlock{ false };
		std::atomic<double> g_phaseMs{ 0.0 };
		std::int64_t        g_next{ 0 };          // present thread: the limiter's next slot
		std::uint64_t       g_hostFrame{ 0 };     // the last HostState seen and the Elden Ring frame period from them
		std::int64_t        g_hostQpc{ 0 };
		double              g_hostPeriod{ 0 };
		Window              g_waited;             // ms our limiter waited per frame

		// The next slot on Elden Ring's frame grid (its HostState stamps, one per frame at the same task) shifted by the
		// phase; 0 when there is no fresh grid.
		std::int64_t GridSlot(std::int64_t a_now)
		{
			proto::HostState hs{};
			if (!link::HostAlive() || !link::ReadHostState(hs) || !hs.qpc) {
				return 0;
			}
			if (hs.frame > g_hostFrame && g_hostFrame && hs.qpc > g_hostQpc) {
				const double per = static_cast<double>(hs.qpc - g_hostQpc) / static_cast<double>(hs.frame - g_hostFrame);
				if (per > 0.004 * static_cast<double>(g_freq) && per < 0.05 * static_cast<double>(g_freq)) {
					g_hostPeriod = g_hostPeriod > 0 ? g_hostPeriod + (per - g_hostPeriod) * 0.05 : per;
				}
			}
			if (hs.frame != g_hostFrame) {
				g_hostFrame = hs.frame;
				g_hostQpc = hs.qpc;
			}
			if (g_hostPeriod <= 0 || a_now - g_hostQpc > static_cast<std::int64_t>(g_freq / 4)) {
				return 0;
			}
			const double base = static_cast<double>(g_hostQpc) + g_phaseMs.load() * static_cast<double>(g_freq) / 1000.0;
			const double k = std::ceil((static_cast<double>(a_now) - base) / g_hostPeriod);
			return static_cast<std::int64_t>(base + k * g_hostPeriod);
		}

		void WaitUntil(std::int64_t a_target)
		{
			// Sleep while more than ~2 ms remain (Wine's Sleep overshoots by ~0.1-1 ms), then spin to the slot.
			for (;;) {
				const std::int64_t left = a_target - Qpc();
				if (left <= 0) {
					return;
				}
				const double ms = Ms(left);
				if (ms > 2.5) {
					::Sleep(static_cast<DWORD>(ms - 2.0));
				} else {
					::YieldProcessor();
				}
			}
		}

		// MGSV's job system: every worker (and the frame's own threads) polls for work with Sleep(1) -- 1.06 ms under Wine
		// (live: ~25 threads, ~800 calls/s each). `!pace jobsleep <us>` makes MGSV's own Sleep(1) a shorter delay.
		using SleepFn = void(WINAPI*)(DWORD);
		SleepFn          g_realSleep{ nullptr };
		std::atomic<int> g_jobSleepUs{ 0 };  // 0 = MGSV's Sleep(1) as it is ([Pacing] JobSleepUs)

		void WINAPI SleepHook(DWORD a_ms)
		{
			const int us = g_jobSleepUs.load(std::memory_order_relaxed);
			if (a_ms == 1 && us > 0 && g_realDelay) {
				LARGE_INTEGER t;
				t.QuadPart = -static_cast<LONGLONG>(us) * 10;
				g_realDelay(FALSE, &t);
				return;
			}
			g_realSleep(a_ms);
		}

		bool HookSleepImport()
		{
			auto* slot = reinterpret_cast<void**>(addr::kSleepIat);
			auto* real = reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "Sleep"));
			if (!real || *slot != real) {
				Log::Warn("pace: {:#x} is not MGSV's kernel32 Sleep import -- no jobsleep", addr::kSleepIat);
				return false;
			}
			g_realSleep = reinterpret_cast<SleepFn>(real);
			DWORD old = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
				return false;
			}
			const auto prior = ::InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&SleepHook), real);
			::VirtualProtect(slot, sizeof(void*), old, &old);
			return prior == real;
		}

		std::string Where(void* a_p)
		{
			HMODULE m = nullptr;
			if (!a_p || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_p), &m)) {
				return std::format("{}", a_p);
			}
			wchar_t path[MAX_PATH]{};
			::GetModuleFileNameW(m, path, MAX_PATH);
			std::wstring name(path);
			name = name.substr(name.find_last_of(L"\\/") + 1);
			std::string n;
			for (wchar_t c : name) {
				n += static_cast<char>(c);
			}
			if (n == "mgsvtpp.exe") {
				return std::format("{}", a_p);
			}
			return std::format("{}+{:#x}", n, reinterpret_cast<std::uintptr_t>(a_p) - reinterpret_cast<std::uintptr_t>(m));
		}
	}

	bool Install()
	{
		LARGE_INTEGER f;
		::QueryPerformanceFrequency(&f);
		g_freq = f.QuadPart;
		HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
		auto*   delay = ntdll ? reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtDelayExecution")) : nullptr;
		g_realDelay = reinterpret_cast<DelayFn>(delay);  // replaced by the trampoline if the wait probe detours it
		bool probe = false;
		if (Cfg().waitProbe && ntdll && delay) {
			// Diagnostics only ([Pacing] WaitProbe): detours under every wait of the process, idle unless `!pace start`.
			auto* one = reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtWaitForSingleObject"));
			auto* many = reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtWaitForMultipleObjects"));
			probe = one && many && Hooks::Create("NtDelayExecution", delay, reinterpret_cast<void*>(&DelayHook), reinterpret_cast<void**>(&g_realDelay));
			probe = probe && Hooks::Create("NtWaitForSingleObject", one, reinterpret_cast<void*>(&WaitOneHook), reinterpret_cast<void**>(&g_realWaitOne));
			probe = probe && Hooks::Create("NtWaitForMultipleObjects", many, reinterpret_cast<void*>(&WaitManyHook), reinterpret_cast<void**>(&g_realWaitMany));
			if (auto* alert = reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtWaitForAlertByThreadId")); probe && alert) {
				Hooks::Create("NtWaitForAlertByThreadId", alert, reinterpret_cast<void*>(&AlertWaitHook), reinterpret_cast<void**>(&g_realAlertWait));
			}
		}
		const bool sleeps = g_realDelay && HookSleepImport();
		if (sleeps) {
			g_jobSleepUs = Cfg().jobSleepUs;
		}
		Log::Info("pace: MGSV's Sleep(1) {}; wait probe {}",
			!sleeps ? std::string("left alone (import not verified)") : g_jobSleepUs.load() ? std::format("= {} us ([Pacing] JobSleepUs)", g_jobSleepUs.load()) : std::string("= 1 ms (MGSV's own)"),
			probe ? "installed (`!pace start [all]`)" : (Cfg().waitProbe ? "refused" : "off ([Pacing] WaitProbe)"));
		return sleeps;
	}

	unsigned BeforePresent(unsigned a_sync)
	{
		const int    sync = g_syncOverride.load(std::memory_order_relaxed);
		const double hz = g_limitHz.load(std::memory_order_relaxed);
		if (hz > 1.0) {
			const auto         now = Qpc();
			const std::int64_t period = static_cast<std::int64_t>(static_cast<double>(g_freq) / hz);
			std::int64_t       slot = g_genlock.load() ? GridSlot(now) : 0;
			if (!slot) {
				g_next = g_next && now - g_next < period ? g_next + period : now;  // free-running at the rate, resync after a stall
				slot = g_next;
			}
			WaitUntil(slot);
			if (g_recording.load(std::memory_order_relaxed)) {
				Spin lock;
				g_waited.Add(Ms(Qpc() - now));
			}
		}
		return sync >= 0 ? static_cast<unsigned>(sync) : a_sync;
	}

	void OnGameFrame(std::int64_t a_qpc)
	{
		g_gameTid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		if (!g_recording.load(std::memory_order_relaxed)) {
			g_lastGame = a_qpc;
			return;
		}
		Spin lock;
		if (g_lastGame) {
			g_gameFrame.Add(Ms(a_qpc - g_lastGame));
			g_gameWaitPerFrame.Add(g_waitThisFrame);
		}
		g_waitThisFrame = 0;
		g_lastGame = a_qpc;
	}

	void OnPresent(std::int64_t a_begin, std::int64_t a_end, std::int64_t a_ours, unsigned a_sync)
	{
		g_presentTid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		if (!g_recording.load(std::memory_order_relaxed)) {
			g_lastPresent = a_begin;
			return;
		}
		Spin lock;
		if (g_lastPresent) {
			g_presentInterval.Add(Ms(a_begin - g_lastPresent));
		}
		g_lastPresent = a_begin;
		g_presentCall.Add(Ms(a_end - a_begin));
		g_ours.Add(Ms(a_ours));
		++g_sync[std::min(a_sync, 4u)];
	}

	std::string Command(std::string_view a_args)
	{
		while (!a_args.empty() && a_args.front() == ' ') {
			a_args.remove_prefix(1);
		}
		if (a_args == "start" || a_args == "start all") {
			g_allThreads = a_args == "start all";
			Spin lock;
			g_sites.clear();
			g_gameFrame = {};
			g_presentInterval = {};
			g_presentCall = {};
			g_ours = {};
			g_gameWaitPerFrame = {};
			g_waited = {};
			g_sync = {};
			g_lastGame = g_lastPresent = 0;
			g_waitThisFrame = 0;
			g_recording = true;
			return "pace: recording";
		}
		if (a_args == "stop") {
			g_recording = false;
			return "pace: stopped";
		}
		// pace sync <0..4|game> | pace limit <hz|off> [grid [phase ms]] | pace jobsleep <us|off>
		if (a_args.starts_with("jobsleep ")) {
			const auto v = a_args.substr(9);
			g_jobSleepUs = v.starts_with("off") ? 0 : std::clamp(std::atoi(std::string(v).c_str()), 0, 1000);
			return std::format("pace: MGSV's Sleep(1) = {}", g_jobSleepUs.load() ? std::format("{} us", g_jobSleepUs.load()) : std::string("1 ms (its own)"));
		}
		if (a_args.starts_with("sync ")) {
			const auto v = a_args.substr(5);
			g_syncOverride = v == "game" ? -1 : std::clamp(std::atoi(std::string(v).c_str()), 0, 4);
			return std::format("pace: present sync interval {}", g_syncOverride.load() < 0 ? std::string("MGSV's own") : std::to_string(g_syncOverride.load()));
		}
		if (a_args.starts_with("limit ")) {
			std::string rest(a_args.substr(6));
			double      hz = rest.starts_with("off") ? 0.0 : std::atof(rest.c_str());
			g_genlock = rest.find("grid") != std::string::npos;
			if (const auto at = rest.find("grid"); at != std::string::npos && rest.size() > at + 4) {
				g_phaseMs = std::atof(rest.c_str() + at + 4);
			}
			g_next = 0;
			g_limitHz = hz;
			return std::format("pace: limiter {} Hz{}", hz, g_genlock ? std::format(" on Elden Ring's frame grid, phase {:.1f} ms", g_phaseMs.load()) : std::string());
		}
		std::vector<WaitSite> sites;
		std::string           out;
		{
			Spin lock;
			out = std::format("pace: {} | game thread {} present thread {}\n game frame interval {}\n game thread waits per frame {}\n present interval {}\n present call {}\n our present work {}\n sync intervals 0:{} 1:{} 2:{} 3:{} 4+:{} (override {})\n limiter {} Hz{} waited {} | Elden Ring period {:.3f} ms\n",
				g_recording ? "recording" : "idle", g_gameTid.load(), g_presentTid.load(), g_gameFrame.Describe(), g_gameWaitPerFrame.Describe(), g_presentInterval.Describe(),
				g_presentCall.Describe(), g_ours.Describe(), g_sync[0], g_sync[1], g_sync[2], g_sync[3], g_sync[4], g_syncOverride.load(), g_limitHz.load(),
				g_genlock ? std::format(" grid phase {:.1f} ms", g_phaseMs.load()) : std::string(), g_waited.Describe(), g_hostPeriod * 1000.0 / static_cast<double>(g_freq ? g_freq : 1));
			sites = g_sites;
		}
		std::sort(sites.begin(), sites.end(), [](const auto& a, const auto& b) { return a.waitedMs > b.waitedMs; });
		static constexpr const char* kKinds[] = { "delay", "wait1", "waitN", "alert" };
		// Per thread its three longest-waiting call chains: the frame limiter is one thread's wait among MGSV's ~24.
		std::vector<DWORD> tids;
		for (const auto& s : sites) {
			if (std::find(tids.begin(), tids.end(), s.tid) == tids.end()) {
				tids.push_back(s.tid);
			}
		}
		for (std::size_t k = 0; k < std::min<std::size_t>(tids.size(), 40); ++k) {
			int shown = 0;
			for (const auto& s : sites) {
				if (s.tid != tids[k] || shown++ >= 3) {
					continue;
				}
				out += std::format(" tid {}{} {} x{} asked {:.2f} waited {:.2f} ms/call (max {:.2f}) total {:.0f} ms:", s.tid,
					s.tid == g_gameTid.load() ? " (game)" : (s.tid == g_presentTid.load() ? " (present)" : ""), kKinds[s.kind], s.calls,
					s.calls ? s.askedMs / static_cast<double>(s.calls) : 0.0, s.calls ? s.waitedMs / static_cast<double>(s.calls) : 0.0, s.maxMs, s.waitedMs);
				for (void* p : s.chain) {
					if (p) {
						out += " " + Where(p);
					}
				}
				out += "\n";
			}
		}
		// The whole list goes next to the DLL too (a console answer holds 4 KB).
		wchar_t path[MAX_PATH]{};
		HMODULE self = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Command), &self) &&
			::GetModuleFileNameW(self, path, MAX_PATH)) {
			std::wstring p(path);
			p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"PhantomRingGuest.pace.txt";
			if (HANDLE f = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
				DWORD written = 0;
				::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
				::CloseHandle(f);
			}
		}
		return out;
	}
}
