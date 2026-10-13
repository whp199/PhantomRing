#include "Platform.h"

#include "Mapping.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace phantom::platform
{
	namespace
	{
		std::wstring Env(const wchar_t* a_name)
		{
			wchar_t    buf[256];
			const auto n = ::GetEnvironmentVariableW(a_name, buf, 256);
			return n > 0 && n < 256 ? std::wstring(buf, n) : std::wstring();
		}

		std::string Lower(std::string_view a_text)
		{
			std::string out;
			for (const char c : a_text) {
				if (c != ' ' && c != '\t') {
					out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
			return out;
		}
	}

	const Facts& Detect()
	{
		static const Facts facts = [] {
			Facts f;
			f.wine = RunningUnderWine();
			const auto forced = Env(L"PHANTOMRING_PLATFORM");
			if (forced == L"windows") {
				f.wine = false;
			} else if (forced == L"wine") {
				f.wine = true;
			}
			// gamescope exports this to everything it starts; Wine passes the Unix environment through.
			f.gamescope = !Env(L"GAMESCOPE_WAYLAND_DISPLAY").empty();
			return f;
		}();
		return facts;
	}

	std::wstring DefaultMapping(bool a_wine) { return a_wine ? kWineMapping : kWindowsMapping; }

	std::wstring ResolveMapping(std::wstring_view a_ini, std::wstring_view a_env, bool a_wine)
	{
		if (!a_env.empty()) {
			return std::wstring(a_env);
		}
		if (a_ini.empty() || a_ini == L"auto") {
			return DefaultMapping(a_wine);
		}
		return std::wstring(a_ini);
	}

	bool Switch(std::string_view a_setting, bool a_autoValue)
	{
		const auto s = Lower(a_setting);
		if (s == "true" || s == "1" || s == "yes" || s == "on") {
			return true;
		}
		if (s == "false" || s == "0" || s == "no" || s == "off") {
			return false;
		}
		return a_autoValue;  // "auto", empty, or anything unknown
	}

	bool HideGuestWindow(std::string_view a_setting, const Facts& a_facts) { return Switch(a_setting, !a_facts.wine && !a_facts.gamescope); }

	bool HostStartsGuest(std::string_view a_setting, const Facts& a_facts) { return Switch(a_setting, !a_facts.wine); }
}
