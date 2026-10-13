#include "Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include <windows.h>

namespace phantom
{
	std::string Narrow(std::wstring_view a_wide)
	{
		if (a_wide.empty()) {
			return {};
		}
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), out.data(), n, nullptr, nullptr);
		return out;
	}

	// Adapted from SkyCraft's Launcher.cpp (MIT, Copyright (c) 2026 chasmlol); see THIRD-PARTY-NOTICES.md.
	std::wstring Widen(std::string_view a_utf8)
	{
		if (a_utf8.empty()) {
			return {};
		}
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(n), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), out.data(), n);
		return out;
	}

	namespace Log
	{
		namespace
		{
			std::mutex        g_lock;
			FILE*             g_file{ nullptr };
			std::string       g_tag;
			std::atomic<bool> g_debug{ false };

			const char* Name(Level a_level)
			{
				switch (a_level) {
				case Level::kDebug:
					return "debug";
				case Level::kInfo:
					return "info";
				case Level::kWarn:
					return "warn";
				default:
					return "error";
				}
			}
		}

		void Init(const std::wstring& a_path, std::string_view a_tag)
		{
			std::scoped_lock lock(g_lock);
			if (g_file) {
				std::fclose(g_file);
			}
			g_file = ::_wfopen(a_path.c_str(), L"wb");
			g_tag = a_tag;
		}

		void SetDebug(bool a_on) { g_debug.store(a_on, std::memory_order_relaxed); }
		bool DebugEnabled() { return g_debug.load(std::memory_order_relaxed); }

		void Write(Level a_level, std::string_view a_text)
		{
			SYSTEMTIME t;
			::GetLocalTime(&t);
			std::scoped_lock lock(g_lock);
			if (!g_file) {
				return;
			}
			std::fprintf(g_file, "[%02u:%02u:%02u.%03u] [%s] [%s] %.*s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, g_tag.c_str(),
				Name(a_level), static_cast<int>(a_text.size()), a_text.data());
			std::fflush(g_file);
		}
	}
}
