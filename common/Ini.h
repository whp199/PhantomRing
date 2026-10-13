#pragma once

#include <map>
#include <string>

namespace phantom
{
	// Minimal INI reader: [Section] / key = value / ; and # comments (also trailing " ;" comments).
	// Keys are case-insensitive. Unknown keys are ignored, missing keys take the given default.
	class Ini
	{
	public:
		bool Load(const std::wstring& a_path);

		[[nodiscard]] std::string Get(const std::string& a_section, const std::string& a_key, const std::string& a_default = {}) const;
		[[nodiscard]] long long   GetInt(const std::string& a_section, const std::string& a_key, long long a_default) const;
		[[nodiscard]] double      GetFloat(const std::string& a_section, const std::string& a_key, double a_default) const;
		[[nodiscard]] bool        GetBool(const std::string& a_section, const std::string& a_key, bool a_default) const;

	private:
		std::map<std::string, std::string> values_;  // "section.key" (lower case) -> value
	};

	// Directory of the module containing this code (with trailing backslash).
	std::wstring ModuleDir();
}
