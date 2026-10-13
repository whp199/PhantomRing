#include "SteamLibrary.h"

#include "Log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace phantom::er::steam
{
	namespace
	{
		bool SameKey(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		class Reader
		{
		public:
			explicit Reader(std::string_view a_text) : text_(a_text) {}

			// Parses "key" (value | { children }) pairs until '}' or the end.
			bool Block(std::vector<KeyValues>& a_out, bool a_nested)
			{
				for (;;) {
					Skip();
					if (pos_ >= text_.size()) {
						return !a_nested;
					}
					if (text_[pos_] == '}') {
						++pos_;
						return a_nested;
					}
					KeyValues kv;
					if (!Token(kv.key)) {
						return false;
					}
					Skip();
					if (pos_ < text_.size() && text_[pos_] == '{') {
						++pos_;
						if (!Block(kv.children, true)) {
							return false;
						}
					} else if (!Token(kv.value)) {
						return false;
					}
					a_out.push_back(std::move(kv));
				}
			}

		private:
			void Skip()
			{
				while (pos_ < text_.size()) {
					if (std::isspace(static_cast<unsigned char>(text_[pos_]))) {
						++pos_;
					} else if (text_.substr(pos_, 2) == "//") {
						while (pos_ < text_.size() && text_[pos_] != '\n') {
							++pos_;
						}
					} else {
						break;
					}
				}
			}

			bool Token(std::string& a_out)
			{
				if (pos_ >= text_.size()) {
					return false;
				}
				if (text_[pos_] != '"') {
					// Unquoted token (rare in Steam's files): up to whitespace or a brace.
					const auto start = pos_;
					while (pos_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[pos_])) && text_[pos_] != '{' && text_[pos_] != '}') {
						++pos_;
					}
					a_out.assign(text_.substr(start, pos_ - start));
					return pos_ > start;
				}
				++pos_;
				while (pos_ < text_.size() && text_[pos_] != '"') {
					char c = text_[pos_++];
					if (c == '\\' && pos_ < text_.size()) {
						const char e = text_[pos_++];
						c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
					}
					a_out += c;
				}
				if (pos_ >= text_.size()) {
					return false;  // unterminated string
				}
				++pos_;
				return true;
			}

			std::string_view text_;
			std::size_t      pos_{ 0 };
		};

		std::wstring RegString(HKEY a_root, const wchar_t* a_key, const wchar_t* a_value)
		{
			wchar_t buf[1024];
			DWORD   bytes = sizeof(buf);
			if (::RegGetValueW(a_root, a_key, a_value, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) {
				return {};
			}
			return std::wstring(buf);
		}

		std::string ReadFile(const std::wstring& a_path)
		{
			std::ifstream in(std::filesystem::path(a_path), std::ios::binary);
			if (!in) {
				return {};
			}
			std::ostringstream all;
			all << in.rdbuf();
			return all.str();
		}

		bool IsFile(const std::wstring& a_path)
		{
			const DWORD attr = ::GetFileAttributesW(a_path.c_str());
			return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
		}

		std::wstring Backslashes(std::wstring a_path)
		{
			std::ranges::replace(a_path, L'/', L'\\');
			while (!a_path.empty() && a_path.back() == L'\\') {
				a_path.pop_back();
			}
			return a_path;
		}
	}

	const KeyValues* KeyValues::Child(std::string_view a_key) const
	{
		for (const auto& c : children) {
			if (SameKey(c.key, a_key)) {
				return &c;
			}
		}
		return nullptr;
	}

	std::optional<KeyValues> Parse(std::string_view a_text)
	{
		KeyValues root;
		Reader    reader(a_text);
		if (!reader.Block(root.children, false)) {
			return std::nullopt;
		}
		return root;
	}

	std::vector<std::string> LibraryFolders(std::string_view a_vdf)
	{
		std::vector<std::string> out;
		const auto               root = Parse(a_vdf);
		const KeyValues*         folders = root ? root->Child("libraryfolders") : nullptr;
		if (!folders) {
			folders = root ? root->Child("LibraryFolders") : nullptr;
		}
		if (!folders) {
			return out;
		}
		for (const auto& entry : folders->children) {
			if (entry.key.empty() || !std::ranges::all_of(entry.key, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
				continue;  // "contentstatsid" and friends
			}
			if (const auto* path = entry.Child("path"); path && !path->value.empty()) {
				out.push_back(path->value);
			} else if (!entry.value.empty()) {
				out.push_back(entry.value);  // old format: "1" "D:\\SteamLibrary"
			}
		}
		return out;
	}

	std::optional<std::string> InstallDir(std::string_view a_manifest)
	{
		const auto root = Parse(a_manifest);
		const auto* state = root ? root->Child("AppState") : nullptr;
		const auto* dir = state ? state->Child("installdir") : nullptr;
		if (!dir || dir->value.empty()) {
			return std::nullopt;
		}
		return dir->value;
	}

	std::wstring SteamRoot()
	{
		auto root = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
		if (root.empty()) {
			root = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
		}
		if (root.empty()) {
			root = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
		}
		return Backslashes(root);
	}

	std::wstring FindAppExe(std::uint32_t a_appId, std::wstring_view a_exe, std::wstring_view a_fallbackDir, std::wstring a_steamRoot, std::string* a_why)
	{
		if (a_steamRoot.empty()) {
			a_steamRoot = SteamRoot();
		}
		if (a_steamRoot.empty()) {
			if (a_why) {
				*a_why = "Steam is not installed (no Valve\\Steam key in the registry)";
			}
			return {};
		}
		a_steamRoot = Backslashes(a_steamRoot);
		std::vector<std::wstring> libraries{ a_steamRoot };
		for (const auto& folder : LibraryFolders(ReadFile(a_steamRoot + L"\\steamapps\\libraryfolders.vdf"))) {
			auto path = Backslashes(Widen(folder));
			if (std::ranges::none_of(libraries, [&](const std::wstring& l) { return ::CompareStringOrdinal(l.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL; })) {
				libraries.push_back(std::move(path));
			}
		}
		std::string tried;
		for (const auto& library : libraries) {
			const auto manifest = ReadFile(std::format(L"{}\\steamapps\\appmanifest_{}.acf", library, a_appId));
			std::wstring dir = manifest.empty() ? std::wstring() : Widen(InstallDir(manifest).value_or(""));
			if (dir.empty()) {
				dir = a_fallbackDir;
			}
			const auto exe = std::format(L"{}\\steamapps\\common\\{}\\{}", library, dir, a_exe);
			if (IsFile(exe)) {
				return exe;
			}
			tried += (tried.empty() ? "" : ", ") + Narrow(exe);
		}
		if (a_why) {
			*a_why = std::format("not in any of {} Steam libraries (tried {})", libraries.size(), tried);
		}
		return {};
	}
}
