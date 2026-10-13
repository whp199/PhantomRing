#include "Ini.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace phantom
{
	namespace
	{
		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		std::string Trim(const std::string& a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return a_text.substr(first, last - first + 1);
		}
	}

	bool Ini::Load(const std::wstring& a_path)
	{
		std::ifstream in(std::filesystem::path(a_path), std::ios::binary);
		if (!in) {
			return false;
		}
		std::string section;
		std::string line;
		while (std::getline(in, line)) {
			if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) {
				line.erase(0, 3);  // UTF-8 BOM
			}
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}
			if (line.front() == '[') {
				const auto end = line.find(']');
				section = Lower(Trim(line.substr(1, end == std::string::npos ? std::string::npos : end - 1)));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			std::string value = line.substr(eq + 1);
			for (const char* marker : { " ;", "\t;", " #", "\t#" }) {
				if (const auto pos = value.find(marker); pos != std::string::npos) {
					value.erase(pos);
				}
			}
			values_[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(value);
		}
		return true;
	}

	std::string Ini::Get(const std::string& a_section, const std::string& a_key, const std::string& a_default) const
	{
		const auto it = values_.find(Lower(a_section) + "." + Lower(a_key));
		return it == values_.end() ? a_default : it->second;
	}

	long long Ini::GetInt(const std::string& a_section, const std::string& a_key, long long a_default) const
	{
		const auto text = Get(a_section, a_key);
		if (text.empty()) {
			return a_default;
		}
		char*      end = nullptr;
		const auto value = std::strtoll(text.c_str(), &end, 0);
		return end && end != text.c_str() ? value : a_default;
	}

	double Ini::GetFloat(const std::string& a_section, const std::string& a_key, double a_default) const
	{
		const auto text = Get(a_section, a_key);
		if (text.empty()) {
			return a_default;
		}
		char*      end = nullptr;
		const auto value = std::strtod(text.c_str(), &end);
		return end && end != text.c_str() ? value : a_default;
	}

	bool Ini::GetBool(const std::string& a_section, const std::string& a_key, bool a_default) const
	{
		const auto text = Lower(Get(a_section, a_key));
		if (text.empty()) {
			return a_default;
		}
		return text == "1" || text == "true" || text == "yes" || text == "on";
	}

	std::wstring ModuleDir()
	{
		HMODULE self = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
		wchar_t    path[MAX_PATH * 4];
		const auto n = ::GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
		std::wstring dir(path, n);
		const auto   slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash + 1);
	}
}
