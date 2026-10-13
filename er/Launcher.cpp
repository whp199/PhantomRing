#include "Launcher.h"

#include "GuestInstall.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Platform.h"
#include "SteamLibrary.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <format>
#include <memory>
#include <mutex>

namespace phantom::er::Launcher
{
	namespace
	{
		std::mutex  g_lock;
		std::string g_state{ "not run" };

		void SetState(std::string a_text, bool a_warn = false)
		{
			if (a_warn) {
				Log::Warn("launcher: {}", a_text);
			} else {
				Log::Info("launcher: {}", a_text);
			}
			std::scoped_lock lock(g_lock);
			g_state = std::move(a_text);
		}

		bool SameName(const std::wstring& a, const std::wstring& b) { return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }

		struct Job
		{
			Settings     settings;
			std::wstring mapping;
		};

		bool WaitForGuest(int a_ms)
		{
			const auto end = ::GetTickCount64() + static_cast<std::uint64_t>(std::max(0, a_ms));
			do {
				if (Link::Get().GuestAliveNow()) {
					return true;
				}
				::Sleep(100);
			} while (::GetTickCount64() < end);
			return Link::Get().GuestAliveNow();
		}

		// Until the guest answers, MGSV exits, or 15 min (its unattended boot takes ~4).
		void Watch(HANDLE a_process, std::uint32_t a_pid)
		{
			const auto started = ::GetTickCount64();
			while (::GetTickCount64() - started < 15 * 60 * 1000) {
				if (::WaitForSingleObject(a_process, 1000) == WAIT_OBJECT_0) {
					DWORD code = 0;
					::GetExitCodeProcess(a_process, &code);
					SetState(std::format("MGSV (pid {}) exited after {} s with code {:#x} before its guest answered: is Steam running and signed in? "
										 "MGSV's own log/crash dialog may say more (start it once from Steam to check)",
								 a_pid, (::GetTickCount64() - started) / 1000, code),
						true);
					return;
				}
				if (Link::Get().GuestAliveNow()) {
					SetState(std::format("MGSV (pid {}) is up, its guest answered after {} s (boot to the sandbox takes a few minutes)", a_pid,
						(::GetTickCount64() - started) / 1000));
					return;
				}
			}
			SetState(std::format("MGSV (pid {}) runs but its guest never answered: see PhantomRingGuest.log in MGSV's folder", a_pid), true);
		}

		DWORD WINAPI Run(void* a_job)
		{
			const std::unique_ptr<Job> job(static_cast<Job*>(a_job));
			const auto&                s = job->settings;
			if (WaitForGuest(s.waitGuestMs)) {
				SetState("an MGSV guest is already running: using it");
				return 0;
			}
			if (const auto running = FindProcesses(s.exeName); !running.empty()) {
				SetState(std::format("{} is running (pid {}) but no PhantomRing guest answers: it was started without PhantomRing "
									 "(e.g. from Steam). Close MGSV and restart Elden Ring",
							 Narrow(s.exeName), running.front()),
					true);
				return 0;
			}
			std::string  why;
			std::wstring exe = s.mgsvExe;
			if (exe.empty()) {
				exe = steam::FindAppExe(kMgsvAppId, s.exeName, L"MGS_TPP", L"", &why);
				if (exe.empty()) {
					SetState("MGSV not found through Steam (" + why + "): set [Launch] MgsvExe in PhantomRingHost.ini", true);
					return 0;
				}
			} else if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
				SetState(std::format("[Launch] MgsvExe = {} does not exist", Narrow(exe)), true);
				return 0;
			}
			const auto dir = exe.substr(0, exe.find_last_of(L"\\/"));
			Log::Info("launcher: MGSV is {}", Narrow(exe));
			if (s.installGuest) {
				auto source = ModuleDir() + L"mgsv";
				const auto result = guestinstall::Install(source, dir);
				if (!result.ok) {
					SetState(result.text, true);
					return 0;
				}
				Log::Info("launcher: {}", result.text);
			}
			const auto    env = BuildEnvironment(nullptr, GuestEnvironment(job->mapping));
			std::uint32_t pid = 0;
			void*         process = nullptr;
			if (!StartProcess(exe, s.mgsvArgs, dir, env, &pid, &process, &why)) {
				SetState("could not start MGSV: " + why, true);
				return 0;
			}
			SetState(std::format("MGSV started (pid {}), hidden; waiting for its guest", pid));
			Watch(static_cast<HANDLE>(process), pid);
			::CloseHandle(static_cast<HANDLE>(process));
			return 0;
		}
	}

	Settings Load(const std::wstring& a_iniPath)
	{
		Settings s;
		Ini      ini;
		ini.Load(a_iniPath);
		s.startMgsv = ini.Get("Launch", "StartMgsv", s.startMgsv);
		s.mgsvExe = Widen(ini.Get("Launch", "MgsvExe"));
		s.mgsvArgs = Widen(ini.Get("Launch", "MgsvArgs"));
		s.installGuest = ini.GetBool("Launch", "InstallGuest", s.installGuest);
		s.waitGuestMs = static_cast<int>(ini.GetInt("Launch", "WaitGuestMs", s.waitGuestMs));
		return s;
	}

	void Start(const Settings& a_settings, const std::wstring& a_mapping)
	{
		const auto& facts = platform::Detect();
		if (!platform::HostStartsGuest(a_settings.startMgsv, facts)) {
			SetState(std::format("not starting MGSV ([Launch] StartMgsv = {}, wine {}: `pr launch` starts it under Linux)", a_settings.startMgsv, facts.wine));
			return;
		}
		auto* job = new Job{ a_settings, a_mapping };
		if (HANDLE t = ::CreateThread(nullptr, 0, &Run, job, 0, nullptr)) {
			::CloseHandle(t);
		} else {
			delete job;
			SetState(std::format("no launcher thread ({})", ::GetLastError()), true);
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return "launcher: " + g_state;
	}

	std::wstring BuildEnvironment(const wchar_t* a_current, const std::vector<std::pair<std::wstring, std::wstring>>& a_set)
	{
		wchar_t*       own = a_current ? nullptr : ::GetEnvironmentStringsW();
		const wchar_t* p = a_current ? a_current : own;
		std::vector<std::wstring> vars;
		for (; p && *p; p += std::wcslen(p) + 1) {
			const std::wstring entry(p);
			// "=C:=C:\dir" (per-drive cwd) entries start with '=': their name runs to the second '='.
			const auto         eq = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
			const std::wstring name = entry.substr(0, eq);
			if (std::ranges::none_of(a_set, [&](const auto& kv) { return SameName(kv.first, name); })) {
				vars.push_back(entry);
			}
		}
		if (own) {
			::FreeEnvironmentStringsW(own);
		}
		for (const auto& [name, value] : a_set) {
			vars.push_back(name + L"=" + value);
		}
		// CreateProcess wants the block sorted by name, case-insensitively.
		std::ranges::sort(vars, [](const std::wstring& a, const std::wstring& b) {
			return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
		});
		std::wstring block;
		for (const auto& v : vars) {
			block += v;
			block += L'\0';
		}
		block += L'\0';
		return block;
	}

	std::vector<std::pair<std::wstring, std::wstring>> GuestEnvironment(const std::wstring& a_mapping)
	{
		const auto app = std::to_wstring(kMgsvAppId);
		return {
			{ L"PHANTOMRING", L"1" },             // the guest stays a plain dinput8 proxy without it
			{ L"PHANTOMRING_ROLE", L"guest" },
			{ L"PHANTOMRING_MAPPING", a_mapping },  // the host's, even if its ini names another
			{ L"SteamAppId", app },                // Elden Ring's own (1245620) would otherwise be inherited
			{ L"SteamGameId", app },
			{ L"SteamOverlayGameId", app },
		};
	}

	bool StartProcess(const std::wstring& a_exe, const std::wstring& a_args, const std::wstring& a_dir, const std::wstring& a_environment,
		std::uint32_t* a_pid, void** a_process, std::string* a_why)
	{
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_SHOWNOACTIVATE;  // its first ShowWindow: never activate (and never minimised)
		std::wstring        command = L"\"" + a_exe + L"\"" + (a_args.empty() ? L"" : L" " + a_args);
		PROCESS_INFORMATION pi{};
		DWORD               flags = CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | CREATE_BREAKAWAY_FROM_JOB;
		auto                env = a_environment;
		BOOL                ok = ::CreateProcessW(a_exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, env.empty() ? nullptr : env.data(),
							   a_dir.empty() ? nullptr : a_dir.c_str(), &si, &pi);
		if (!ok && ::GetLastError() == ERROR_ACCESS_DENIED) {
			// Elden Ring's job doesn't allow breakaway: MGSV then lives as long as that job.
			flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
			command = L"\"" + a_exe + L"\"" + (a_args.empty() ? L"" : L" " + a_args);
			ok = ::CreateProcessW(a_exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, env.empty() ? nullptr : env.data(),
				a_dir.empty() ? nullptr : a_dir.c_str(), &si, &pi);
		}
		if (!ok) {
			if (a_why) {
				*a_why = std::format("CreateProcess {} failed ({})", Narrow(a_exe), ::GetLastError());
			}
			return false;
		}
		::CloseHandle(pi.hThread);
		if (a_pid) {
			*a_pid = pi.dwProcessId;
		}
		if (a_process) {
			*a_process = pi.hProcess;
		} else {
			::CloseHandle(pi.hProcess);
		}
		return true;
	}

	std::vector<std::uint32_t> FindProcesses(const std::wstring& a_exeName)
	{
		std::vector<std::uint32_t> out;
		HANDLE                     snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return out;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry)) {
			if (SameName(entry.szExeFile, a_exeName)) {
				out.push_back(entry.th32ProcessID);
			}
		}
		::CloseHandle(snapshot);
		return out;
	}
}
