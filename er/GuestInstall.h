#pragma once
// Puts the MGSV guest (dinput8.dll + PhantomRingGuest.ini, shipped in the mgsv\ folder next to
// PhantomRingHost.dll) into MGSV's folder before er/Launcher starts it, on Windows. Same rules as the
// Linux launcher's install_guest (tools/phantomring/launch.py):
//  - a dinput8.dll PhantomRing didn't put there (IHHook, another mod) is never overwritten: MGSV is then
//    not started, and the log says why;
//  - one we put there before (its sha256 is in PhantomRingGuest.installed next to it) is replaced by the
//    shipped one when they differ;
//  - an ini the player edited is kept (only ours, unmodified, is refreshed).
// Every copy is verified by hash before it replaces anything.

#include <string>
#include <vector>

namespace phantom::er::guestinstall
{
	inline constexpr wchar_t kRecordName[] = L"PhantomRingGuest.installed";

	enum class Action
	{
		kNone,      // already the shipped file
		kCopy,      // absent: copy
		kReplace,   // an older one of ours: replace
		kRefuse,    // dll: not ours, leave it and don't start MGSV
		kKeepUser,  // ini: edited by the player (or not ours): keep it
	};
	const char* Name(Action a_action);

	// Pure decisions (hashes are lower-case hex; a_installed empty = no file).
	Action DecideDll(const std::string& a_shipped, const std::string& a_installed, const std::vector<std::string>& a_ours);
	Action DecideIni(const std::string& a_shipped, const std::string& a_installed, const std::vector<std::string>& a_ours);

	std::string Sha256File(const std::wstring& a_path);  // empty if unreadable

	struct Result
	{
		bool        ok{ false };  // the guest in a_mgsvDir is the shipped one (MGSV may start)
		Action      dll{ Action::kNone };
		Action      ini{ Action::kNone };
		std::string text;         // one line for the log
	};
	// a_sourceDir: folder holding the shipped dinput8.dll and PhantomRingGuest.ini; a_mgsvDir: MGSV's folder.
	Result Install(const std::wstring& a_sourceDir, const std::wstring& a_mgsvDir);
}
