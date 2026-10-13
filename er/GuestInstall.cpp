#include "GuestInstall.h"

#include "Log.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace phantom::er::guestinstall
{
	namespace
	{
		bool Contains(const std::vector<std::string>& a_list, const std::string& a_hash) { return std::ranges::find(a_list, a_hash) != a_list.end(); }

		struct Record
		{
			std::vector<std::string> dll;
			std::vector<std::string> ini;
		};

		// "dll <sha256>" / "ini <sha256>" lines: every file PhantomRing ever put in this folder.
		Record ReadRecord(const std::wstring& a_path)
		{
			Record        r;
			std::ifstream in{ std::filesystem::path(a_path) };
			std::string   line;
			while (std::getline(in, line)) {
				std::istringstream fields(line);
				std::string        kind, hash;
				if (fields >> kind >> hash && (kind == "dll" || kind == "ini") && hash.size() == 64) {
					(kind == "dll" ? r.dll : r.ini).push_back(hash);
				}
			}
			return r;
		}

		bool WriteRecord(const std::wstring& a_path, const Record& a_record)
		{
			std::ofstream out(std::filesystem::path(a_path), std::ios::trunc);
			out << "# Files PhantomRing installed here (sha256). Its launcher only replaces or removes these.\n";
			for (const auto& h : a_record.dll) {
				out << "dll " << h << "\n";
			}
			for (const auto& h : a_record.ini) {
				out << "ini " << h << "\n";
			}
			return static_cast<bool>(out);
		}

		// Only a missing file counts as absent; anything else (sharing violation, access denied) exists.
		bool Absent(const std::wstring& a_path)
		{
			if (::GetFileAttributesW(a_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
				return false;
			}
			const DWORD err = ::GetLastError();
			return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
		}

		// Copy to <dst>.new, check its hash, then swap it in (so a half-written dinput8.dll never exists).
		bool CopyVerified(const std::wstring& a_src, const std::wstring& a_dst, const std::string& a_hash, std::string& a_why)
		{
			const auto tmp = a_dst + L".new";
			if (!::CopyFileW(a_src.c_str(), tmp.c_str(), FALSE)) {
				a_why = std::format("copy to {} failed ({})", Narrow(tmp), ::GetLastError());
				return false;
			}
			if (Sha256File(tmp) != a_hash) {
				::DeleteFileW(tmp.c_str());
				a_why = std::format("copy to {} did not verify", Narrow(tmp));
				return false;
			}
			if (!::MoveFileExW(tmp.c_str(), a_dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				a_why = std::format("replacing {} failed ({}; is MGSV running?)", Narrow(a_dst), ::GetLastError());
				::DeleteFileW(tmp.c_str());
				return false;
			}
			return true;
		}
	}

	const char* Name(Action a_action)
	{
		switch (a_action) {
		case Action::kNone:
			return "up to date";
		case Action::kCopy:
			return "installed";
		case Action::kReplace:
			return "updated";
		case Action::kRefuse:
			return "REFUSED (not PhantomRing's)";
		case Action::kKeepUser:
			return "kept (edited)";
		}
		return "?";
	}

	Action DecideDll(const std::string& a_shipped, const std::string& a_installed, const std::vector<std::string>& a_ours)
	{
		if (a_installed.empty()) {
			return Action::kCopy;
		}
		if (a_installed == a_shipped) {
			return Action::kNone;
		}
		return Contains(a_ours, a_installed) ? Action::kReplace : Action::kRefuse;
	}

	Action DecideIni(const std::string& a_shipped, const std::string& a_installed, const std::vector<std::string>& a_ours)
	{
		if (a_installed.empty()) {
			return Action::kCopy;
		}
		if (a_installed == a_shipped) {
			return Action::kNone;
		}
		return Contains(a_ours, a_installed) ? Action::kReplace : Action::kKeepUser;
	}

	std::string Sha256File(const std::wstring& a_path)
	{
		HANDLE file = ::CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return {};
		}
		BCRYPT_ALG_HANDLE  alg = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::string        hex;
		if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
			std::vector<UCHAR> buf(1 << 20);
			DWORD              got = 0;
			bool               ok = true;
			while (ok) {
				if (!::ReadFile(file, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
					ok = false;  // a read error mid-file: no hash, not the hash of the part we got
				} else if (got == 0) {
					break;
				} else {
					ok = ::BCryptHashData(hash, buf.data(), got, 0) >= 0;
				}
			}
			UCHAR digest[32];
			if (ok && ::BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
				for (const UCHAR b : digest) {
					hex += std::format("{:02x}", b);
				}
			}
		}
		if (hash) {
			::BCryptDestroyHash(hash);
		}
		if (alg) {
			::BCryptCloseAlgorithmProvider(alg, 0);
		}
		::CloseHandle(file);
		return hex;
	}

	Result Install(const std::wstring& a_sourceDir, const std::wstring& a_mgsvDir)
	{
		Result     r;
		const auto srcDll = a_sourceDir + L"\\dinput8.dll";
		const auto srcIni = a_sourceDir + L"\\PhantomRingGuest.ini";
		const auto dstDll = a_mgsvDir + L"\\dinput8.dll";
		const auto dstIni = a_mgsvDir + L"\\PhantomRingGuest.ini";
		const auto recordPath = a_mgsvDir + L"\\" + kRecordName;

		const auto shippedDll = Sha256File(srcDll);
		if (shippedDll.empty()) {
			r.text = std::format("guest: {} is missing (the package's mgsv folder belongs next to PhantomRingHost.dll)", Narrow(srcDll));
			return r;
		}
		auto       record = ReadRecord(recordPath);
		const auto dstDllHash = Sha256File(dstDll);
		if (dstDllHash.empty() && !Absent(dstDll)) {
			// No hash is not "no file": a dll we can't read (locked, no read permission) may still be
			// replaceable, and it may be another mod's.
			r.dll = Action::kRefuse;
			r.text = std::format("guest: {} exists but cannot be read; MGSV not started", Narrow(dstDll));
			return r;
		}
		r.dll = DecideDll(shippedDll, dstDllHash, record.dll);
		if (r.dll == Action::kRefuse) {
			r.text = std::format("guest: {} exists and is not PhantomRing's (sha256 {}): another mod? Move it away to use PhantomRing; MGSV not started",
				Narrow(dstDll), Sha256File(dstDll));
			return r;
		}
		std::string why;
		if ((r.dll == Action::kCopy || r.dll == Action::kReplace) && !CopyVerified(srcDll, dstDll, shippedDll, why)) {
			r.text = "guest: " + why;
			return r;
		}
		if (!Contains(record.dll, shippedDll)) {
			record.dll.push_back(shippedDll);
		}

		const auto shippedIni = Sha256File(srcIni);
		if (shippedIni.empty()) {
			r.ini = Action::kKeepUser;  // nothing shipped: the guest runs with its built-in defaults or the player's
		} else {
			const auto dstIniHash = Sha256File(dstIni);
			if (dstIniHash.empty() && !Absent(dstIni)) {
				r.ini = Action::kKeepUser;  // unreadable is not absent: never overwrite the player's ini blind
				Log::Warn("guest: {} exists but cannot be read; left as it is", Narrow(dstIni));
			} else {
				r.ini = DecideIni(shippedIni, dstIniHash, record.ini);
			}
			if ((r.ini == Action::kCopy || r.ini == Action::kReplace) && !CopyVerified(srcIni, dstIni, shippedIni, why)) {
				Log::Warn("guest: {} (the guest keeps its previous ini or defaults)", why);
			}
			if (!Contains(record.ini, shippedIni)) {
				record.ini.push_back(shippedIni);
			}
		}
		if (!WriteRecord(recordPath, record)) {
			Log::Warn("guest: could not write {}", Narrow(recordPath));
		}
		r.ok = true;
		r.text = std::format("guest in {}: dinput8.dll {} (sha256 {}...), PhantomRingGuest.ini {}", Narrow(a_mgsvDir), Name(r.dll), shippedDll.substr(0, 16), Name(r.ini));
		return r;
	}
}
