#pragma once

#include <format>
#include <string>
#include <string_view>

namespace phantom
{
	std::string  Narrow(std::wstring_view a_wide);
	std::wstring Widen(std::string_view a_utf8);

	// A plain append-only log file (one per process), flushed every line so a crash keeps it.
	namespace Log
	{
		enum class Level
		{
			kDebug,
			kInfo,
			kWarn,
			kError,
		};

		// a_path: where to write (truncated). Lines before Init go nowhere.
		void Init(const std::wstring& a_path, std::string_view a_tag);
		void SetDebug(bool a_on);
		bool DebugEnabled();
		void Write(Level a_level, std::string_view a_text);

		template <class... Args>
		void Debug(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (DebugEnabled()) {
				Write(Level::kDebug, std::format(a_fmt, std::forward<Args>(a_args)...));
			}
		}
		template <class... Args>
		void Info(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			Write(Level::kInfo, std::format(a_fmt, std::forward<Args>(a_args)...));
		}
		template <class... Args>
		void Warn(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			Write(Level::kWarn, std::format(a_fmt, std::forward<Args>(a_args)...));
		}
		template <class... Args>
		void Error(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			Write(Level::kError, std::format(a_fmt, std::forward<Args>(a_args)...));
		}
	}
}
