#pragma once
// Where Steam installed a game, on Windows: the Steam folder from the registry, every library folder from
// steamapps\libraryfolders.vdf, then the app's steamapps\appmanifest_<id>.acf ("installdir") in each.
// The Linux launcher does the same in tools/phantomring/launch.py (Machine.libraries / app_dir).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::er::steam
{
	// Valve KeyValues text ("key" "value", "key" { ... }, // comments, \\ \" \n \t escapes).
	struct KeyValues
	{
		std::string            key;
		std::string            value;
		std::vector<KeyValues> children;

		[[nodiscard]] const KeyValues* Child(std::string_view a_key) const;  // case-insensitive
	};
	std::optional<KeyValues> Parse(std::string_view a_text);

	// libraryfolders.vdf -> library roots (UTF-8, as written). Also understands the pre-2021 form
	// ("1" "D:\\SteamLibrary").
	std::vector<std::string> LibraryFolders(std::string_view a_vdf);
	// appmanifest_<id>.acf -> its "installdir" (folder name under steamapps\common).
	std::optional<std::string> InstallDir(std::string_view a_manifest);

	// The Steam folder: HKCU\Software\Valve\Steam SteamPath, else HKLM ...\Valve\Steam InstallPath.
	std::wstring SteamRoot();

	// Full path of a_exe in app a_appId's install folder, searching every library of a_steamRoot (empty =
	// SteamRoot()). Falls back to steamapps\common\<a_fallbackDir> when no manifest names one.
	// Empty + a_why when not found.
	std::wstring FindAppExe(std::uint32_t a_appId, std::wstring_view a_exe, std::wstring_view a_fallbackDir, std::wstring a_steamRoot, std::string* a_why);
}
