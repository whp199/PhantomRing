#include "SaveGuard.h"

#include "Config.h"
#include "Hook.h"
#include "Ini.h"
#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace phantom::guest::saves
{
	namespace
	{
		// ---- files ------------------------------------------------------------------------------
		using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		using DeleteFileWFn = BOOL(WINAPI*)(LPCWSTR);
		using MoveFileWithProgressWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, DWORD);

		CreateFileWFn           g_createFileW{ nullptr };
		DeleteFileWFn           g_deleteFileW{ nullptr };
		MoveFileWithProgressWFn g_moveFile{ nullptr };

		std::mutex            g_fileLock;
		std::set<std::wstring> g_redirected;  // lower-case basenames written to the sink this session
		std::wstring          g_sink;
		thread_local bool     t_inside = false;  // our own CreateDirectory/CreateFile calls

		std::wstring Lower(std::wstring a_s)
		{
			for (auto& c : a_s) {
				c = static_cast<wchar_t>(std::towlower(c));
			}
			return a_s;
		}

		std::wstring BaseName(const std::wstring& a_path)
		{
			const auto slash = a_path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? a_path : a_path.substr(slash + 1);
		}

		bool LooksLikeSave(const std::wstring& a_lowerPath)
		{
			if (a_lowerPath.find(L"\\userdata\\") != std::wstring::npos || a_lowerPath.find(L"/userdata/") != std::wstring::npos) {
				return true;
			}
			const auto base = BaseName(a_lowerPath);
			return base.starts_with(L"tpp_") || base.starts_with(L"mgo_") || base.starts_with(L"personal_data");
		}

		bool IsWrite(DWORD a_access, DWORD a_disposition)
		{
			constexpr DWORD kWriteBits = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
			return (a_access & kWriteBits) || a_disposition == CREATE_ALWAYS || a_disposition == TRUNCATE_EXISTING || a_disposition == CREATE_NEW;
		}

		// The sink path for a save-like file, or empty when a_path should be left alone.
		std::wstring Redirect(LPCWSTR a_path, bool a_write)
		{
			if (!a_path || t_inside || g_sink.empty()) {
				return {};
			}
			const std::wstring lower = Lower(a_path);
			if (lower.starts_with(Lower(g_sink)) || !LooksLikeSave(lower)) {
				return {};
			}
			const auto       base = BaseName(lower);
			std::scoped_lock lock(g_fileLock);
			if (a_write) {
				if (g_redirected.insert(base).second) {
					Log::Info("saves: redirected write of {} to the sink", Narrow(a_path));
				}
			} else if (!g_redirected.contains(base)) {
				return {};  // reading the real save is fine (Continue needs it)
			}
			return g_sink + BaseName(std::wstring(a_path));
		}

		HANDLE WINAPI CreateFileWDetour(LPCWSTR a_path, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_sa, DWORD a_disposition, DWORD a_flags, HANDLE a_template)
		{
			const auto target = Redirect(a_path, IsWrite(a_access, a_disposition));
			return g_createFileW(target.empty() ? a_path : target.c_str(), a_access, a_share, a_sa, a_disposition, a_flags, a_template);
		}

		BOOL WINAPI DeleteFileWDetour(LPCWSTR a_path)
		{
			const auto target = Redirect(a_path, true);
			if (!target.empty()) {
				Log::Info("saves: blocked delete of {}", Narrow(a_path));
				g_deleteFileW(target.c_str());
				return TRUE;
			}
			return g_deleteFileW(a_path);
		}

		BOOL WINAPI MoveFileDetour(LPCWSTR a_from, LPCWSTR a_to, LPPROGRESS_ROUTINE a_progress, LPVOID a_data, DWORD a_flags)
		{
			const auto to = Redirect(a_to, true);
			if (!to.empty()) {
				auto from = Redirect(a_from, true);
				Log::Info("saves: redirected rename {} -> {}", a_from ? Narrow(a_from) : std::string("?"), Narrow(a_to));
				return g_moveFile(from.empty() ? a_from : from.c_str(), to.c_str(), a_progress, a_data, a_flags | MOVEFILE_REPLACE_EXISTING);
			}
			return g_moveFile(a_from, a_to, a_progress, a_data, a_flags);
		}

		// ---- Steam remote storage (v012) -----------------------------------------------------------
		enum Slot : std::size_t
		{
			kFileWrite = 0,
			kFileRead = 1,
			kFileForget = 2,
			kFileDelete = 3,
			kStreamOpen = 6,
			kStreamChunk = 7,
			kStreamClose = 8,
			kStreamCancel = 9,
			kFileExists = 10,
			kGetFileSize = 12,
		};

		using FileWriteFn = bool (*)(void*, const char*, const void*, std::int32_t);
		using FileReadFn = std::int32_t (*)(void*, const char*, void*, std::int32_t);
		using NameBoolFn = bool (*)(void*, const char*);
		using SizeFn = std::int32_t (*)(void*, const char*);

		FileReadFn  g_fileRead{ nullptr };
		NameBoolFn  g_fileExists{ nullptr };
		SizeFn      g_fileSize{ nullptr };
		std::atomic<bool> g_steamDone{ false };

		std::mutex                                     g_steamLock;
		std::map<std::string, std::vector<std::uint8_t>> g_cloud;    // what MGSV "wrote" this session
		std::set<std::string>                          g_deleted;  // what MGSV "deleted" this session
		std::map<std::uint64_t, std::pair<std::string, std::vector<std::uint8_t>>> g_streams;
		std::uint64_t                                  g_nextStream{ 0x50480000'00000001ull };

		bool SteamFileWrite(void*, const char* a_name, const void* a_data, std::int32_t a_size)
		{
			if (!a_name || a_size < 0) {
				return false;
			}
			std::scoped_lock lock(g_steamLock);
			auto&            buf = g_cloud[a_name];
			buf.assign(static_cast<const std::uint8_t*>(a_data), static_cast<const std::uint8_t*>(a_data) + a_size);
			g_deleted.erase(a_name);
			Log::Info("saves: dropped Steam Cloud write {} ({} bytes)", a_name, a_size);
			return true;
		}

		std::int32_t SteamFileRead(void* a_self, const char* a_name, void* a_out, std::int32_t a_size)
		{
			{
				std::scoped_lock lock(g_steamLock);
				if (a_name) {
					if (auto it = g_cloud.find(a_name); it != g_cloud.end()) {
						const auto n = std::min<std::int32_t>(a_size, static_cast<std::int32_t>(it->second.size()));
						std::memcpy(a_out, it->second.data(), static_cast<std::size_t>(n));
						return n;
					}
					if (g_deleted.contains(a_name)) {
						return 0;
					}
				}
			}
			return g_fileRead(a_self, a_name, a_out, a_size);
		}

		bool SteamForgetOrDelete(void*, const char* a_name)
		{
			std::scoped_lock lock(g_steamLock);
			if (a_name) {
				g_cloud.erase(a_name);
				g_deleted.insert(a_name);
				Log::Info("saves: dropped Steam Cloud delete/forget of {}", a_name);
			}
			return true;
		}

		bool SteamFileExists(void* a_self, const char* a_name)
		{
			{
				std::scoped_lock lock(g_steamLock);
				if (a_name && g_cloud.contains(a_name)) {
					return true;
				}
				if (a_name && g_deleted.contains(a_name)) {
					return false;
				}
			}
			return g_fileExists(a_self, a_name);
		}

		std::int32_t SteamFileSize(void* a_self, const char* a_name)
		{
			{
				std::scoped_lock lock(g_steamLock);
				if (a_name) {
					if (auto it = g_cloud.find(a_name); it != g_cloud.end()) {
						return static_cast<std::int32_t>(it->second.size());
					}
					if (g_deleted.contains(a_name)) {
						return 0;
					}
				}
			}
			return g_fileSize(a_self, a_name);
		}

		std::uint64_t SteamStreamOpen(void*, const char* a_name)
		{
			std::scoped_lock lock(g_steamLock);
			const auto       h = g_nextStream++;
			g_streams[h] = { a_name ? a_name : "", {} };
			return h;
		}

		bool SteamStreamChunk(void*, std::uint64_t a_handle, const void* a_data, std::int32_t a_size)
		{
			std::scoped_lock lock(g_steamLock);
			auto             it = g_streams.find(a_handle);
			if (it == g_streams.end() || a_size < 0) {
				return false;
			}
			const auto* p = static_cast<const std::uint8_t*>(a_data);
			it->second.second.insert(it->second.second.end(), p, p + a_size);
			return true;
		}

		bool SteamStreamClose(void*, std::uint64_t a_handle)
		{
			std::scoped_lock lock(g_steamLock);
			auto             it = g_streams.find(a_handle);
			if (it == g_streams.end()) {
				return false;
			}
			Log::Info("saves: dropped Steam Cloud stream write {} ({} bytes)", it->second.first, it->second.second.size());
			g_cloud[it->second.first] = std::move(it->second.second);
			g_deleted.erase(it->second.first);
			g_streams.erase(it);
			return true;
		}

		bool SteamStreamCancel(void*, std::uint64_t a_handle)
		{
			std::scoped_lock lock(g_steamLock);
			return g_streams.erase(a_handle) > 0;
		}
	}

	bool InstallFileGuards()
	{
		g_sink = ModuleDir() + Cfg().saveSinkDir + L"\\";
		t_inside = true;
		::CreateDirectoryW(g_sink.c_str(), nullptr);
		t_inside = false;
		HMODULE kernel = ::GetModuleHandleW(L"kernelbase.dll");
		if (!kernel) {
			kernel = ::GetModuleHandleW(L"kernel32.dll");
		}
		auto* create = reinterpret_cast<void*>(::GetProcAddress(kernel, "CreateFileW"));
		auto* del = reinterpret_cast<void*>(::GetProcAddress(kernel, "DeleteFileW"));
		auto* move = reinterpret_cast<void*>(::GetProcAddress(kernel, "MoveFileWithProgressW"));
		bool  ok = create && Hooks::Create("CreateFileW", create, reinterpret_cast<void*>(&CreateFileWDetour), reinterpret_cast<void**>(&g_createFileW));
		ok &= del && Hooks::Create("DeleteFileW", del, reinterpret_cast<void*>(&DeleteFileWDetour), reinterpret_cast<void**>(&g_deleteFileW));
		ok &= move && Hooks::Create("MoveFileWithProgressW", move, reinterpret_cast<void*>(&MoveFileDetour), reinterpret_cast<void**>(&g_moveFile));
		if (!ok) {
			Log::Error("saves: file guards incomplete -- MGSV may write its local save files");
		}
		Log::Info("saves: file writes to save paths go to {}", Narrow(g_sink));
		return ok;
	}

	SteamPoll PollSteam()
	{
		if (g_steamDone.load()) {
			return SteamPoll::kWaiting;
		}
		HMODULE api = ::GetModuleHandleW(L"steam_api64.dll");
		if (!api) {
			return SteamPoll::kWaiting;
		}
		using AccessorFn = void* (*)();
		const auto accessor = reinterpret_cast<AccessorFn>(reinterpret_cast<void*>(::GetProcAddress(api, "SteamRemoteStorage")));
		void*      storage = accessor ? accessor() : nullptr;
		if (!storage) {
			return SteamPoll::kWaiting;  // SteamAPI_Init hasn't run yet
		}
		auto** vtable = *static_cast<void***>(storage);
		g_fileRead = reinterpret_cast<FileReadFn>(vtable[kFileRead]);
		g_fileExists = reinterpret_cast<NameBoolFn>(vtable[kFileExists]);
		g_fileSize = reinterpret_cast<SizeFn>(vtable[kGetFileSize]);
		bool ok = true;
		ok &= Hooks::SwapVtable(vtable, kFileWrite, reinterpret_cast<void*>(&SteamFileWrite)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kFileRead, reinterpret_cast<void*>(&SteamFileRead)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kFileForget, reinterpret_cast<void*>(&SteamForgetOrDelete)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kFileDelete, reinterpret_cast<void*>(&SteamForgetOrDelete)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kStreamOpen, reinterpret_cast<void*>(&SteamStreamOpen)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kStreamChunk, reinterpret_cast<void*>(&SteamStreamChunk)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kStreamClose, reinterpret_cast<void*>(&SteamStreamClose)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kStreamCancel, reinterpret_cast<void*>(&SteamStreamCancel)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kFileExists, reinterpret_cast<void*>(&SteamFileExists)) != nullptr;
		ok &= Hooks::SwapVtable(vtable, kGetFileSize, reinterpret_cast<void*>(&SteamFileSize)) != nullptr;
		g_steamDone.store(true);
		if (ok) {
			Log::Info("saves: ISteamRemoteStorage {} guarded (writes kept in memory, nothing reaches Steam Cloud)", storage);
			return SteamPoll::kGuarded;
		}
		Log::Error("saves: could not swap every ISteamRemoteStorage slot -- Steam Cloud writes may get through");
		return SteamPoll::kIncomplete;
	}

	bool SteamGuarded() { return g_steamDone.load(); }
}
